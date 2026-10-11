/**
 * @file      link_service.cpp
 * @brief     Transport-agnostic PagerLink service core (see link_service.h).
 *
 * All state lives in one process-global instance guarded by a
 * std::recursive_mutex. The Reassembler callbacks run while that lock is held
 * and re-enter the RPC layer (and, through it, the handlers) — the recursive
 * mutex lets a handler call service_packet_size() without deadlocking.
 */
#include "hal/link/link_service.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "hal/link/link_xfer.h"
#include "hal/power.h"
#include "hal/storage.h"

#ifdef ARDUINO
#include <esp32-hal-log.h>
#define LINK_LOG(...) log_i(__VA_ARGS__)
#else
#include <cstdio>
#define LINK_LOG(fmt, ...) printf("[link] " fmt "\n", ##__VA_ARGS__)
#endif

namespace pagerlink {
namespace {

std::recursive_mutex g_mutex;

struct State {
    bool connected = false;
    std::unique_ptr<Rpc> rpc;
    Segmenter segmenter;

    /** The transport's packet writer, valid while connected. */
    std::function<bool(const uint8_t *pkt, size_t len)> send_packet;

    /* Counters for the current connection. */
    uint32_t rx_packets = 0;
    uint32_t tx_packets = 0;
    uint32_t rx_messages = 0;

    /* Running totals folded in on disconnect, so they survive reconnects. */
    uint32_t total_resets = 0;
    uint32_t total_malformed = 0;
    uint32_t total_timeouts = 0;

    size_t packet_size = 0;
    std::string peer_name;

    /* Battery-change baseline for evt.battery. */
    int last_batt_pct = 0;
    bool last_charging = false;
    bool have_batt = false;

    /* hw_get_filesystem_generation() last announced with evt.fs.changed. */
    uint32_t last_fs_gen = 0;

    /* Outbound (pager -> phone) bulk transfers started by service_send_bytes().
     * The phone allocated the even xfer id in the RPC that announced them. */
    struct Outbound {
        uint16_t xfer;
        std::unique_ptr<XferSender> tx;
        std::function<void(bool)> done;
        uint32_t last_ms;    /* steady_ms() at the start or the last ACK progress */
        uint32_t stall_ms;   /* no-ACK-progress timeout checked by service_tick() */
        uint32_t last_acked; /* acked() at that last progress */
    };
    std::vector<Outbound> outbound;
};

State &state() {
    static State s;
    return s;
}

/**
 * Event subscriptions. A function-static registry so it lives across
 * reconnects — a subscriber keeps receiving the same event after the phone
 * drops and comes back. Guarded by g_mutex (recursive: a subscriber callback
 * may unsubscribe).
 */
struct Subscribers {
    std::vector<std::pair<int, std::pair<std::string, std::function<void(const std::string &)>>>>
        subs;
    int next_token = 1;
};

Subscribers &subscribers() {
    static Subscribers s;
    return s;
}

/** Milliseconds from a monotonic clock, wrapping at 2^32 (Rpc's ClockFn). */
uint32_t steady_ms() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/**
 * Finish outbound transfer `i`: take its completion callback and erase the
 * entry BEFORE invoking it, so the callback may start another transfer.
 * Runs with g_mutex held (recursive, so the callback may re-enter).
 */
void finish_outbound(size_t i, bool ok) {
    State &s = state();
    std::function<void(bool)> done = std::move(s.outbound[i].done);
    const uint16_t xfer = s.outbound[i].xfer;
    s.outbound.erase(s.outbound.begin() + static_cast<std::ptrdiff_t>(i));
    LINK_LOG("send %s id=%u", ok ? "done" : "failed", static_cast<unsigned>(xfer));
    if (done) {
        done(ok);
    }
}

/** Fail every outbound transfer (disconnect / defensive teardown). */
void outbound_reset() {
    while (!state().outbound.empty()) {
        finish_outbound(0, false);
    }
}

/** Route an ACK to an outbound transfer; anything else is not ours. */
void outbound_on_bulk(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len) {
    if (type != MsgType::Ack) {
        return;
    }
    State &s = state();
    for (size_t i = 0; i < s.outbound.size(); ++i) {
        if (s.outbound[i].xfer != xfer) {
            continue;
        }
        State::Outbound &o = s.outbound[i];
        const uint32_t before = o.tx->acked();
        o.tx->on_ack(payload, len);
        if (o.tx->acked() != before) {
            o.last_ms = steady_ms();
            o.last_acked = o.tx->acked();
        }
        const XferState st = o.tx->pump();
        if (st == XferState::Done) {
            finish_outbound(i, true);
        } else if (st == XferState::Failed) {
            finish_outbound(i, false);
        }
        return;
    }
}

/**
 * Reassembler callbacks. They run inside service_on_packet() with g_mutex held
 * and only touch the current Rpc — never a service_* entry point that would
 * re-lock the lifecycle functions.
 */
void on_message(const uint8_t *msg, size_t len) {
    State &s = state();
    s.rx_messages++;
    if (s.rpc) {
        s.rpc->on_message(msg, len);
    }
}

void on_reset(ResetReason r) {
    State &s = state();
    if (s.rpc) {
        s.rpc->on_link_reset(r);
    }
    LINK_LOG("link reset: %s", reset_reason_name(r));
}

Reassembler &reassembler() {
    static Reassembler r(on_message, on_reset);
    return r;
}

}  // namespace

void service_on_connect(const TransportOps &ops) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();

    reassembler().reset();
    s.segmenter.reset();

    /* Fold any counters left over from a connection we never saw disconnect
     * for (defensive; normally service_on_disconnect() already did this). */
    if (s.rpc) {
        s.connected = false; /* done() callbacks must not start a new transfer */
        outbound_reset();
        fs_reset();
        s.total_resets += s.rpc->stats().link_resets;
        s.total_malformed += s.rpc->stats().malformed;
        s.total_timeouts += s.rpc->stats().timeouts;
        s.rpc.reset();
    }

    s.send_packet = ops.send_packet;
    s.packet_size = ops.packet_size;
    s.peer_name = ops.peer_name;
    s.rx_packets = 0;
    s.tx_packets = 0;
    s.rx_messages = 0;

    s.rpc.reset(new Rpc(
        Role::Pager,
        [](const std::string &message) -> bool {
            State &st = state();
            if (!st.send_packet) {
                return false;
            }
            return st.segmenter.send(
                reinterpret_cast<const uint8_t *>(message.data()), message.size(),
                st.packet_size, [&st](const uint8_t *pkt, size_t len) -> bool {
                    if (!st.send_packet(pkt, len)) {
                        return false;
                    }
                    st.tx_packets++;
                    return true;
                });
        },
        []() -> uint32_t { return steady_ms(); }));

    register_sys_handlers(*s.rpc);
    register_fs_handlers(*s.rpc);
#ifndef ARDUINO
    register_test_handlers(*s.rpc);
#endif

    /* Bulk passthrough: the fs.* transfers claim their ids first; everything
     * else may be an outbound transfer the pager started. */
    s.rpc->on_bulk([](MsgType type, uint16_t xfer, const uint8_t *payload, size_t len) {
        if (!fs_on_bulk(type, xfer, payload, len)) {
            outbound_on_bulk(type, xfer, payload, len);
        }
    });

    /* Event fan-out. Copy the matching subscribers first, then call them, so a
     * callback may (un)subscribe without disturbing the walk. */
    s.rpc->on_event(
        [](const std::string &event, const JsonObject &params) {
            std::vector<std::function<void(const std::string &)>> fns;
            Subscribers &subs = subscribers();
            for (const auto &entry : subs.subs) {
                if (entry.second.first == event) {
                    fns.push_back(entry.second.second);
                }
            }
            for (const std::function<void(const std::string &)> &fn : fns) {
                if (fn) {
                    fn(params.text());
                }
            }
        });

    s.last_fs_gen = hw_get_filesystem_generation();

    monitor_params_t mp;
    hw_get_monitor_params(mp);
    s.last_batt_pct = mp.battery_percent;
    s.last_charging = mp.is_charging;
    s.have_batt = true;

    s.connected = true;
    LINK_LOG("connected: %s", s.peer_name.c_str());
}

void service_on_packet(const uint8_t *pkt, size_t len) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();
    if (!s.connected) {
        return;
    }
    s.rx_packets++;
    reassembler().feed(pkt, len);
}

void service_on_disconnect() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();

    s.connected = false; /* refuse new calls; done() must not restart a transfer */
    outbound_reset();

    if (s.rpc) {
        fs_reset();
        s.rpc->disconnect();
        s.total_resets += s.rpc->stats().link_resets;
        s.total_malformed += s.rpc->stats().malformed;
        s.total_timeouts += s.rpc->stats().timeouts;
        s.rpc.reset();
    }
    s.send_packet = nullptr;

    LINK_LOG("disconnected: %s", s.peer_name.c_str());
}

void service_tick() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return;
    }

    s.rpc->tick();
    fs_tick(steady_ms());

    /* Outbound sends that saw no ACK progress for their stall window fail. */
    const uint32_t now = steady_ms();
    for (size_t i = 0; i < s.outbound.size();) {
        if (static_cast<uint32_t>(now - s.outbound[i].last_ms) > s.outbound[i].stall_ms) {
            LINK_LOG("send stalled id=%u", static_cast<unsigned>(s.outbound[i].xfer));
            finish_outbound(i, false); /* erases index i; do not advance */
        } else {
            ++i;
        }
    }

    /* Any local write (editor save, link upload, eviction) bumps the
     * generation; tell the phone to drop its listing cache. */
    const uint32_t gen = hw_get_filesystem_generation();
    if (gen != s.last_fs_gen) {
        s.last_fs_gen = gen;
        s.rpc->emit("evt.fs.changed", "{}");
    }

    monitor_params_t mp;
    hw_get_monitor_params(mp);
    const int pct = mp.battery_percent;
    const bool charging = mp.is_charging;

    int delta = pct - s.last_batt_pct;
    if (delta < 0) {
        delta = -delta;
    }
    if (!s.have_batt || delta >= 5 || charging != s.last_charging) {
        JsonWriter w;
        w.begin_object()
            .key("pct")
            .int_val(pct)
            .key("charging")
            .bool_val(charging)
            .end_object();
        s.rpc->emit("evt.battery", w.str());
        s.last_batt_pct = pct;
        s.last_charging = charging;
        s.have_batt = true;
    }
}

bool service_connected() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return state().connected;
}

std::string service_peer_name() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();
    return s.connected ? s.peer_name : std::string();
}

void service_get_stats(HwLinkStats &out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();
    out.rx_packets = s.rx_packets;
    out.tx_packets = s.tx_packets;
    out.rx_messages = s.rx_messages;
    out.link_resets = s.total_resets + (s.rpc ? s.rpc->stats().link_resets : 0);
    out.malformed = s.total_malformed + (s.rpc ? s.rpc->stats().malformed : 0);
    out.timeouts = s.total_timeouts + (s.rpc ? s.rpc->stats().timeouts : 0);
}

size_t service_packet_size() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return state().packet_size;
}

bool service_call(const std::string &method, const std::string &params_json, uint32_t timeout_ms,
                  Rpc::ResponseFn cb) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return false;
    }
    return s.rpc->call(method, params_json, std::move(cb), timeout_ms);
}

bool service_send_bytes(uint16_t xfer, const uint8_t *data, size_t len, uint32_t stall_ms,
                        std::function<void(bool ok)> done) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return false;
    }
    /* Outbound transfers use the even id the phone allocated. */
    if (xfer == 0 || (xfer % 2) != 0) {
        return false;
    }
    if (!data && len != 0) {
        return false;
    }
    for (const State::Outbound &o : s.outbound) {
        if (o.xfer == xfer) {
            return false; /* already an outbound transfer */
        }
    }

    State::Outbound o;
    o.xfer = xfer;
    o.done = std::move(done);
    o.stall_ms = stall_ms;
    o.last_ms = steady_ms();
    o.last_acked = 0;
    o.tx.reset(new XferSender(
        static_cast<uint32_t>(len), MAX_DATA_CHUNK,
        [data](uint32_t off, uint8_t *buf, size_t n) -> bool {
            std::memcpy(buf, data + off, n);
            return true;
        },
        [xfer](const std::string &payload) -> bool {
            State &st = state();
            return st.rpc && st.rpc->send_bulk(MsgType::Data, xfer, payload);
        },
        0));
    s.outbound.push_back(std::move(o));

    LINK_LOG("send start id=%u size=%u", static_cast<unsigned>(xfer),
             static_cast<unsigned>(len));

    const size_t i = s.outbound.size() - 1;
    const XferState st = s.outbound[i].tx->pump();
    if (st == XferState::Done) {
        finish_outbound(i, true);
    } else if (st == XferState::Failed) {
        finish_outbound(i, false);
    }
    return true; /* true even when done already ran */
}

bool service_emit(const std::string &event, const std::string &params_json) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return false;
    }
    return s.rpc->emit(event, params_json.empty() ? "{}" : params_json);
}

int service_subscribe(const std::string &event,
                      std::function<void(const std::string &params_json)> fn) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Subscribers &subs = subscribers();
    const int token = subs.next_token++;
    subs.subs.emplace_back(token, std::make_pair(event, std::move(fn)));
    return token;
}

void service_unsubscribe(int token) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    Subscribers &subs = subscribers();
    for (size_t i = 0; i < subs.subs.size(); ++i) {
        if (subs.subs[i].first == token) {
            subs.subs.erase(subs.subs.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

}  // namespace pagerlink
