/**
 * @file      link_rpc.h
 * @brief     PagerLink v1 RPC layer — REQ/RES/ERR/EVT dispatch with timeouts.
 *
 * Pure C++ (C++11 on the device, gnu++17 in native_test): no Arduino / ESP-IDF headers, so this compiles on the host for
 * the unit tests as well as on the pager. Implements the message-layer
 * behaviour of PROTOCOL.md §3 on top of the framing codec (link_frame.h) and
 * the JSON helpers (link_json.h): request/response correlation, event
 * fan-out, bulk DATA/ACK passthrough, request-id parity and allocation, and
 * per-request timeouts measured by the caller.
 *
 * NOT thread-safe. The owner, link_service.cpp, serializes every call into
 * this object under its g_mutex: on_message(), tick() and disconnect() run on
 * the link (transport) task; call(), send_bulk() and emit() also run on worker
 * tasks through service_call / service_send_bytes / service_accept_expect /
 * service_emit. Handlers and callbacks run with that lock held.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "hal/link/link_frame.h"
#include "hal/link/link_json.h"
#include "hal/result.h"

namespace pagerlink {

/** Wire `code` string for a HalError (PROTOCOL.md §3.4); Ok and unknown -> "internal". */
const char *hal_error_code(HalError e);

/** Error carried by an ERR payload, or delivered locally on timeout / disconnect. */
struct RpcError {
    std::string code;
    std::string msg;
};

/** Which side of the link this endpoint is; selects request-id parity. */
enum class Role : uint8_t { Phone, Pager };

/** Counters for the failure modes the link layer can observe. */
struct RpcStats {
    uint32_t malformed = 0;      /* undecodable message dropped */
    uint32_t late_responses = 0; /* RES/ERR with no outstanding request */
    uint32_t bad_requests = 0;   /* REQ payload not a JSON object with a string "m" */
    uint32_t unsupported = 0;    /* REQ naming an unregistered method */
    uint32_t timeouts = 0;       /* request expired locally */
    uint32_t link_resets = 0;    /* link-layer reset reported by the owner */
};

/**
 * Correlates outgoing requests with their responses over one link.
 *
 * Outgoing requests get an id of this side's parity (Phone even, Pager odd;
 * never 0) and are dispatched by call(). Incoming REQ messages run the
 * registered handler and are answered with RES or ERR; incoming EVT messages
 * fan out to on_event(); DATA/ACK messages are handed verbatim to on_bulk().
 * A response whose id has no outstanding request is dropped as late.
 */
class Rpc {
public:
    /** Send one encoded message (header + payload); the transport segments it.
     * The bytes are valid only for the call (a reused buffer, P7.36). */
    using SendFn = std::function<bool(const uint8_t *message, size_t len)>;
    /** Milliseconds, wrapping at 2^32; compare with signed `now - deadline` math. */
    using ClockFn = std::function<uint32_t()>;
    /**
     * Handles an incoming REQ. Return true to answer RES with `result` (an
     * empty writer yields "{}"); return false to answer ERR with `err` (an
     * empty err.code is replaced by "internal").
     */
    using Handler = std::function<bool(const JsonObject &params, JsonWriter &result, RpcError &err)>;
    /** Completion of an outgoing request: RES -> ok=true; ERR/timeout/disconnect -> ok=false. */
    using ResponseFn = std::function<void(bool ok, const std::string &result_json, const RpcError &err)>;
    /** An incoming EVT, or the locally generated "link.reset". */
    using EventFn = std::function<void(const std::string &event, const JsonObject &params)>;
    /** A DATA/ACK message forwarded verbatim (`payload` is the raw payload). */
    using BulkFn = std::function<void(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len)>;

    /** Default per-request timeout (PROTOCOL.md §3.3). */
    static constexpr uint32_t DEFAULT_TIMEOUT_MS = 10000;
    /** Maximum outstanding requests; call() fails once this many are live. */
    static constexpr size_t MAX_PENDING = 16;

    Rpc(Role role, SendFn send, ClockFn clock);

    /** Register or replace the handler for `method`. */
    void handle(const std::string &method, Handler h);
    /** Set the single event sink (incoming EVT and the local link.reset). */
    void on_event(EventFn fn);
    /** Set the single bulk-message sink (DATA/ACK passthrough). */
    void on_bulk(BulkFn fn);

    /**
     * Run `fn` once the reply to the REQ being handled has been sent. Meant for
     * handlers that must send follow-up messages (e.g. fs.read's first DATA)
     * only after their RES. Outside a handler, `fn` runs immediately.
     */
    void defer(std::function<void()> fn);

    /**
     * Send a REQ. `params_json` is a JSON object text or empty (empty omits the
     * "p" member). Returns false — and never invokes `cb` — when MAX_PENDING
     * requests are already outstanding, when encoding fails, or when send()
     * returns false.
     */
    bool call(const std::string &method, const std::string &params_json, ResponseFn cb,
              uint32_t timeout_ms = DEFAULT_TIMEOUT_MS);

    /** Send an EVT (id 0) with payload {"e":event,"p":params or {}}. */
    bool emit(const std::string &event, const std::string &params_json);

    /** Send a pre-encoded DATA/ACK message with transfer id `xfer`. */
    bool send_bulk(MsgType type, uint16_t xfer, const std::string &payload);
    /** Same, from a byte span (no std::string on the DATA path, P7.36). */
    bool send_bulk(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len);

    /** Feed one reassembled message (header + payload). */
    void on_message(const uint8_t *msg, size_t len);
    /** Report a link-layer reset; bumps stats and raises the local "link.reset" event. */
    void on_link_reset(ResetReason r);
    /** Expire requests whose deadline has passed, failing each with `timeout`. */
    void tick();
    /** Milliseconds until the nearest request deadline, capped at `cap`; 0 when
     * one is already due (the transport's tick wait, P7.35). */
    uint32_t next_wait_ms(uint32_t cap) const;
    /** Fail every outstanding request with `disconnected`. */
    void disconnect();

    /** Number of outstanding requests. */
    size_t pending() const { return pending_.size(); }
    /** Cumulative counters. */
    const RpcStats &stats() const { return stats_; }

private:
    struct Pending {
        uint16_t id;
        uint32_t deadline;
        ResponseFn cb;
    };

    void handle_req(const MessageView &m);
    void handle_res(const MessageView &m, bool is_result);
    void handle_evt(const MessageView &m);

    bool send_message(MsgType t, uint16_t id, const std::string &payload);
    bool send_message(MsgType t, uint16_t id, const uint8_t *payload, size_t len);
    bool send_err(uint16_t id, const char *code, const std::string &msg);
    bool send_res(uint16_t id, const std::string &result_json);

    uint16_t alloc_id();
    uint16_t advance_id(uint16_t id) const;
    bool id_pending(uint16_t id) const;

    Role role_;
    SendFn send_;
    ClockFn clock_;
    std::map<std::string, Handler> handlers_;
    EventFn on_event_;
    BulkFn on_bulk_;
    std::vector<Pending> pending_;
    std::vector<std::function<void()>> deferred_;
    bool in_handler_ = false;
    /* Encoded outgoing message, reused (MAX_MESSAGE, PSRAM-first; P7.36).
     * tx_busy_ covers a send_ that re-enters send_message(): that nested
     * message uses a temporary buffer instead. */
    ByteBuf tx_;
    bool tx_busy_ = false;
    uint16_t next_id_;
    RpcStats stats_;
};

}  // namespace pagerlink
