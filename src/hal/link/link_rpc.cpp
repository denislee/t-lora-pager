/**
 * @file      link_rpc.cpp
 * @brief     PagerLink v1 RPC layer implementation.
 *
 * Standard library only; mirrors PROTOCOL.md §3. Ids are allocated per role
 * parity and wrap within the parity, skipping any id still outstanding.
 * Expiry and disconnection both erase the pending entry before invoking its
 * callback, so a callback may safely issue a new request.
 */
#include "hal/link/link_rpc.h"

#include <utility>

namespace pagerlink {

const char *hal_error_code(HalError e) {
    switch (e) {
        case HalError::InvalidArgument: return "invalid_argument";
        case HalError::NotSupported:    return "unsupported";
        case HalError::InternalError:   return "internal";
        case HalError::StorageFull:     return "storage_full";
        case HalError::WifiOffline:     return "wifi_offline";
        case HalError::HttpError:       return "http_error";
        case HalError::Unauthorized:    return "unauthorized";
        case HalError::LinkOffline:     return "disconnected";   /* local only, never sent */
        case HalError::HubDisabled:     /* off the wire (HUB_REMOVAL_PLAN H3.1); enumerator goes in H4.2 */
        case HalError::HubUnreachable:
        case HalError::Ok:              break;
    }
    return "internal"; /* Ok, and any value outside the enum, reads as "internal" */
}

Rpc::Rpc(Role role, SendFn send, ClockFn clock)
    : role_(role),
      send_(std::move(send)),
      clock_(std::move(clock)),
      next_id_(role == Role::Phone ? 2 : 1) {}

void Rpc::handle(const std::string &method, Handler h) { handlers_[method] = std::move(h); }
void Rpc::on_event(EventFn fn) { on_event_ = std::move(fn); }
void Rpc::on_bulk(BulkFn fn) { on_bulk_ = std::move(fn); }

void Rpc::defer(std::function<void()> fn) {
    if (in_handler_) {
        deferred_.push_back(std::move(fn));
    } else if (fn) {
        fn();
    }
}

// ---------------------------------------------------------------------------
// Outgoing
// ---------------------------------------------------------------------------

bool Rpc::send_message(MsgType t, uint16_t id, const std::string &payload) {
    return send_message(t, id, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
}

bool Rpc::send_message(MsgType t, uint16_t id, const uint8_t *payload, size_t len) {
    if (tx_busy_) {
        ByteBuf nested;
        return encode_message_into(t, id, payload, len, nested) &&
               send_(nested.data(), nested.size());
    }
    if (!encode_message_into(t, id, payload, len, tx_)) {
        return false;
    }
    tx_busy_ = true;
    const bool ok = send_(tx_.data(), tx_.size());
    tx_busy_ = false;
    return ok;
}

bool Rpc::send_err(uint16_t id, const char *code, const std::string &msg) {
    JsonWriter w;
    w.begin_object()
        .key("code")
        .str_val(code == nullptr ? "" : code)
        .key("msg")
        .str_val(msg)
        .end_object();
    return send_message(MsgType::Err, id, w.str());
}

bool Rpc::send_res(uint16_t id, const std::string &result_json) {
    if (result_json.empty()) {
        return send_message(MsgType::Res, id, "{}");
    }
    return send_message(MsgType::Res, id, result_json);
}

uint16_t Rpc::advance_id(uint16_t id) const {
    uint16_t next = static_cast<uint16_t>(id + 2u); /* wraps mod 2^16 */
    if (next == 0) {
        next = 2; /* Phone: 65534 -> 2 */
    }
    return next;
}

bool Rpc::id_pending(uint16_t id) const {
    for (const Pending &p : pending_) {
        if (p.id == id) {
            return true;
        }
    }
    return false;
}

uint16_t Rpc::alloc_id() {
    uint16_t id = next_id_;
    for (size_t i = 0; i < 32768; ++i) {
        if (!id_pending(id)) {
            next_id_ = advance_id(id);
            return id;
        }
        id = advance_id(id);
    }
    return 0; /* unreachable while MAX_PENDING < 32768 */
}

bool Rpc::call(const std::string &method, const std::string &params_json, ResponseFn cb,
               uint32_t timeout_ms) {
    if (pending_.size() >= MAX_PENDING) {
        return false;
    }

    JsonWriter w;
    w.begin_object().key("m").str_val(method);
    if (!params_json.empty()) {
        w.key("p").raw_val(params_json.c_str());
    }
    w.end_object();

    const uint16_t id = alloc_id();
    if (w.str().size() > MAX_PAYLOAD) {
        return false; /* what encode_message() rejected before the send */
    }

    Pending p;
    p.id = id;
    p.deadline = static_cast<uint32_t>(clock_() + timeout_ms);
    p.cb = std::move(cb);
    pending_.push_back(std::move(p));

    if (!send_message(MsgType::Req, id, reinterpret_cast<const uint8_t *>(w.str().data()),
                      w.str().size())) {
        /* Roll the pending entry back; the id has already been consumed. */
        for (size_t i = 0; i < pending_.size(); ++i) {
            if (pending_[i].id == id) {
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        return false;
    }
    return true;
}

bool Rpc::emit(const std::string &event, const std::string &params_json) {
    JsonWriter w;
    w.begin_object().key("e").str_val(event).key("p");
    if (params_json.empty()) {
        w.begin_object().end_object();
    } else {
        w.raw_val(params_json.c_str());
    }
    w.end_object();
    return send_message(MsgType::Evt, 0, w.str());
}

bool Rpc::send_bulk(MsgType type, uint16_t xfer, const std::string &payload) {
    return send_message(type, xfer, payload);
}

bool Rpc::send_bulk(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len) {
    return send_message(type, xfer, payload, len);
}

// ---------------------------------------------------------------------------
// Incoming
// ---------------------------------------------------------------------------

void Rpc::handle_req(const MessageView &m) {
    /* A REQ must carry the other role's parity (PROTOCOL.md §3.2). */
    const bool need_even = (role_ == Role::Pager);
    const bool parity_ok = need_even ? (m.id != 0 && (m.id & 1u) == 0) : ((m.id & 1u) == 1);
    if (!parity_ok) {
        send_err(m.id, "invalid_argument", "bad request id parity");
        return;
    }

    JsonObject req;
    std::string method;
    if (!req.parse(reinterpret_cast<const char *>(m.payload), m.len) ||
        !req.get_string("m", method)) {
        stats_.bad_requests++;
        send_err(m.id, "bad_request", "bad request");
        return;
    }

    JsonObject params;
    if (req.has("p")) {
        if (!req.get_object("p", params)) {
            send_err(m.id, "bad_request", "bad request");
            return;
        }
    } else {
        params.parse("{}"); /* no params: an empty object (cannot fail) */
    }

    auto it = handlers_.find(method);
    if (it == handlers_.end() || !it->second) {
        stats_.unsupported++;
        send_err(m.id, "unsupported", "unknown method " + method);
        return;
    }

    JsonWriter result;
    RpcError err;
    in_handler_ = true;
    const bool ok = it->second(params, result, err);
    in_handler_ = false;
    if (ok) {
        send_res(m.id, result.str());
    } else {
        if (err.code.empty()) {
            err.code = "internal";
        }
        send_err(m.id, err.code.c_str(), err.msg);
    }

    /* The reply is out (or could not be sent): run any actions the handler
     * queued via defer(), in registration order. Take a local copy first so a
     * deferred action may itself defer again without disturbing this walk. */
    std::vector<std::function<void()>> deferred;
    deferred.swap(deferred_);
    for (std::function<void()> &fn : deferred) {
        if (fn) {
            fn();
        }
    }
}

void Rpc::handle_res(const MessageView &m, bool is_result) {
    size_t idx = pending_.size();
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].id == m.id) {
            idx = i;
            break;
        }
    }
    if (idx == pending_.size()) {
        stats_.late_responses++;
        return;
    }

    /* Erase before invoking so the callback may call() again. */
    ResponseFn cb = std::move(pending_[idx].cb);
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(idx));

    if (is_result) {
        cb(true, std::string(reinterpret_cast<const char *>(m.payload), m.len), RpcError{});
        return;
    }

    RpcError err;
    JsonObject obj;
    if (!obj.parse(reinterpret_cast<const char *>(m.payload), m.len) ||
        !obj.get_string("code", err.code)) {
        err.code = "internal";
    } else {
        obj.get_string("msg", err.msg); /* msg is optional */
    }
    cb(false, "", err);
}

void Rpc::handle_evt(const MessageView &m) {
    JsonObject obj;
    std::string name;
    if (!obj.parse(reinterpret_cast<const char *>(m.payload), m.len) ||
        !obj.get_string("e", name)) {
        stats_.malformed++;
        return;
    }

    JsonObject params;
    if (obj.has("p")) {
        if (!obj.get_object("p", params)) {
            stats_.malformed++;
            return;
        }
    } else {
        params.parse("{}"); /* no params: an empty object (cannot fail) */
    }

    if (on_event_) {
        on_event_(name, params);
    }
}

void Rpc::on_message(const uint8_t *msg, size_t len) {
    /* A view into the reassembler's buffer: valid for this call only; the
     * REQ/RES/EVT paths copy what they keep (JsonObject::parse owns its text). */
    MessageView m;
    if (decode_message_view(msg, len, m) != DecodeError::None) {
        stats_.malformed++;
        return;
    }

    switch (m.type) {
        case MsgType::Req:
            handle_req(m);
            break;
        case MsgType::Res:
            handle_res(m, true);
            break;
        case MsgType::Err:
            handle_res(m, false);
            break;
        case MsgType::Evt:
            handle_evt(m);
            break;
        case MsgType::Data:
        case MsgType::Ack:
            if (on_bulk_) {
                on_bulk_(m.type, m.id, m.payload, m.len);
            }
            break;
    }
}

void Rpc::on_link_reset(ResetReason r) {
    stats_.link_resets++;
    if (!on_event_) {
        return;
    }
    JsonObject params;
    const std::string text = std::string("{\"reason\":\"") + reset_reason_name(r) + "\"}";
    if (params.parse(text)) {
        on_event_("link.reset", params);
    }
}

// ---------------------------------------------------------------------------
// Time / lifecycle
// ---------------------------------------------------------------------------

void Rpc::tick() {
    const uint32_t now = clock_();
    std::vector<Pending> expired;
    std::vector<Pending> keep;
    keep.reserve(pending_.size());
    for (Pending &p : pending_) {
        if (static_cast<int32_t>(now - p.deadline) >= 0) {
            expired.push_back(std::move(p));
        } else {
            keep.push_back(std::move(p));
        }
    }
    pending_.swap(keep);

    for (Pending &p : expired) {
        stats_.timeouts++;
        RpcError err;
        err.code = "timeout";
        err.msg = "request timed out";
        p.cb(false, "", err);
    }
    /* A callback may have added new pendings; those keep their own deadlines. */
}

uint32_t Rpc::next_wait_ms(uint32_t cap) const {
    if (pending_.empty()) {
        return cap;
    }
    const uint32_t now = clock_();
    uint32_t wait = cap;
    for (const Pending &p : pending_) {
        wait = min_wait_ms(wait, now, p.deadline);
    }
    return wait;
}

void Rpc::disconnect() {
    std::vector<Pending> doomed;
    doomed.swap(pending_);
    for (Pending &p : doomed) {
        RpcError err;
        err.code = "disconnected";
        err.msg = "link dropped";
        p.cb(false, "", err);
    }
}

}  // namespace pagerlink
