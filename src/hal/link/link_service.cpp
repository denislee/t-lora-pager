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

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "hal/link/link_xfer.h"
#include "hal/power.h"
#include "hal/storage.h"
#include "core/input_profile.h"

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

/* Lock-free mirror of State::connected, so UI code (the home screen polls
 * hw_link_connected() every second from inside the LVGL frame) never waits for
 * g_mutex while the link task holds it for I2C/BLE work. Written only with
 * g_mutex held, next to every write of State::connected. */
std::atomic<bool> g_connected{false};

/* g_mutex guard. The profile build (INPUT_PROFILE) reports holds >= 50 ms
 * with the holder's function, so a stalled link can be told from a slow UI. */
struct LinkLock {
#ifdef INPUT_PROFILE
    explicit LinkLock(const char *who) : who_(who) {
        g_mutex.lock();
        t0_ = core::prof::now_us();
    }
    ~LinkLock() {
        const uint64_t us = core::prof::now_us() - t0_;
        g_mutex.unlock();
        if (us >= 50000) core::prof::span(who_, us);
    }
    const char *who_;
    uint64_t t0_;
#else
    explicit LinkLock(const char *) { g_mutex.lock(); }
    ~LinkLock() { g_mutex.unlock(); }
#endif
    LinkLock(const LinkLock &) = delete;
    LinkLock &operator=(const LinkLock &) = delete;
};

struct State {
    bool connected = false;
    std::unique_ptr<Rpc> rpc;
    Segmenter segmenter;

    /** The transport's packet writer, valid while connected. */
    std::function<bool(const uint8_t *pkt, size_t len)> send_packet;
    /** The transport's room check (TransportOps::tx_room); may be empty. */
    std::function<bool(size_t packets)> tx_room;
    /** TransportOps::bulk_activity; may be empty. */
    std::function<void()> bulk_activity;
    /** TransportOps::wake; may be empty. */
    std::function<void()> wake;

    /* Counters for the current connection. */
    uint32_t rx_packets = 0;
    uint32_t tx_packets = 0;
    uint32_t rx_messages = 0;

    /* Running totals folded in on disconnect, so they survive reconnects. */
    uint32_t total_resets = 0;
    uint32_t total_malformed = 0;
    uint32_t total_timeouts = 0;

    size_t packet_size = 0;
    size_t rx_queue_packets = 0;
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
        uint32_t rto_ms;     /* steady_ms() at the start, the last ACK, or the last retransmit */
        uint32_t stall_ms;   /* no-ACK-progress timeout checked by service_tick() */
        uint32_t last_acked; /* acked() at that last progress */
    };
    std::vector<Outbound> outbound;

    /* Inbound (phone -> pager) bulk into memory, opened by service_accept_open().
     * The pager allocated the odd xfer id; the phone streams DATA after it
     * announced {xfer,size,crc32}. DATA arriving before service_accept_expect()
     * sets the size is kept in `early` and replayed. */
    struct Inbound {
        uint16_t xfer;
        uint32_t max_size;
        std::vector<std::string> early; /* DATA payloads before expect() */
        size_t early_bytes;             /* sum of early payload sizes */
        bool expecting;                 /* expect() ran; rx is live */
        bool failed;                    /* overflowed before expect() */
        uint32_t crc;                   /* expected CRC-32 of the whole body */
        std::string buf;                /* reassembled bytes */
        std::unique_ptr<XferReceiver> rx;
        std::function<void(bool, std::string &)> done;
        uint32_t last_ms;  /* steady_ms() at expect() or the last DATA */
        uint32_t stall_ms; /* no-DATA timeout checked by service_tick() */
    };
    std::vector<Inbound> inbound;
    uint16_t next_inbound_id = 32769; /* odd; fs.write counts up from 1 */
};

State &state() {
    static State s;
    return s;
}

/** Tell the transport bulk bytes are moving (P7.33). Caller holds g_mutex. */
void note_bulk_activity() {
    State &s = state();
    if (s.bulk_activity) {
        s.bulk_activity();
    }
}

/** A deadline was registered off the transport task (P7.35). Caller holds g_mutex. */
void note_new_deadline() {
    State &s = state();
    if (s.wake) {
        s.wake();
    }
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

/** Index of inbound receiver `xfer`, or -1. */
long inbound_find(uint16_t xfer) {
    State &s = state();
    for (size_t i = 0; i < s.inbound.size(); ++i) {
        if (s.inbound[i].xfer == xfer) return static_cast<long>(i);
    }
    return -1;
}

/**
 * Finish inbound receiver `i` that has a waiter: verify, take the callback and
 * the bytes, and leave the entry in place (spent) for service_accept_close().
 * Runs with g_mutex held.
 */
void finish_inbound(size_t i, bool ok) {
    State &s = state();
    State::Inbound &in = s.inbound[i];
    if (ok) {
        const uint32_t got = crc32_update(0, reinterpret_cast<const uint8_t *>(in.buf.data()),
                                          in.buf.size());
        if (got != in.crc) {
            LINK_LOG("accept crc mismatch id=%u", static_cast<unsigned>(in.xfer));
            ok = false;
        }
    }
    std::function<void(bool, std::string &)> done = std::move(in.done);
    in.done = nullptr;
    in.rx.reset();
    std::string bytes;
    bytes.swap(in.buf);
    LINK_LOG("accept %s id=%u size=%u", ok ? "done" : "failed", static_cast<unsigned>(in.xfer),
             static_cast<unsigned>(bytes.size()));
    if (!ok) bytes.clear();
    if (done) done(ok, bytes);
}

/** Fail every waiting inbound receiver and drop them all (disconnect). */
void inbound_reset() {
    State &s = state();
    for (size_t i = 0; i < s.inbound.size(); ++i) {
        if (s.inbound[i].done) finish_inbound(i, false);
    }
    s.inbound.clear();
}

/** Feed one DATA payload to a live receiver and finish it on Done/Failed. */
void inbound_feed(size_t i, const uint8_t *payload, size_t len) {
    State::Inbound &in = state().inbound[i];
    if (!in.rx || !in.done) return;
    in.last_ms = steady_ms();
    const XferState st = in.rx->on_data(payload, len);
    if (st == XferState::Done) {
        finish_inbound(i, true);
    } else if (st == XferState::Failed) {
        finish_inbound(i, false);
    }
}

/** Route a DATA to an inbound receiver; returns false when the id is not ours. */
bool inbound_on_bulk(MsgType type, uint16_t xfer, const uint8_t *payload, size_t len) {
    const long idx = inbound_find(xfer);
    if (idx < 0) return false;
    if (type != MsgType::Data) return true; /* an ACK on a download: ignore */
    State::Inbound &in = state().inbound[static_cast<size_t>(idx)];
    if (in.expecting) {
        inbound_feed(static_cast<size_t>(idx), payload, len);
        return true;
    }
    if (in.failed) return true;
    /* Not expecting yet: buffer, bounded by max_size plus one chunk's header. */
    if (in.early_bytes + len > static_cast<size_t>(in.max_size) + 4 * (in.early.size() + 1)) {
        LINK_LOG("accept overflow id=%u", static_cast<unsigned>(xfer));
        in.failed = true;
        in.early.clear();
        in.early_bytes = 0;
        return true;
    }
    in.early.emplace_back(reinterpret_cast<const char *>(payload), len);
    in.early_bytes += len;
    return true;
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
        o.rto_ms = steady_ms();
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
    LinkLock lock(__func__);
    State &s = state();

    reassembler().reset();
    s.segmenter.reset();

    /* Fold any counters left over from a connection we never saw disconnect
     * for (defensive; normally service_on_disconnect() already did this). */
    if (s.rpc) {
        s.connected = false; /* done() callbacks must not start a new transfer */
        g_connected.store(false);
        outbound_reset();
        inbound_reset();
        fs_reset();
        s.total_resets += s.rpc->stats().link_resets;
        s.total_malformed += s.rpc->stats().malformed;
        s.total_timeouts += s.rpc->stats().timeouts;
        s.rpc.reset();
    }

    s.send_packet = ops.send_packet;
    s.tx_room = ops.tx_room;
    s.bulk_activity = ops.bulk_activity;
    s.wake = ops.wake;
    s.packet_size = ops.packet_size;
    s.rx_queue_packets = ops.rx_queue_packets;
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
            /* Queue the message whole or not at all (P7.32). */
            if (st.tx_room &&
                !st.tx_room(packets_per_message(message.size(), st.packet_size))) {
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
    register_weather_handlers(*s.rpc);
#ifndef ARDUINO
    register_test_handlers(*s.rpc);
#endif

    /* Bulk passthrough: the fs.* transfers claim their ids first; everything
     * else may be an outbound transfer the pager started. */
    s.rpc->on_bulk([](MsgType type, uint16_t xfer, const uint8_t *payload, size_t len) {
        note_bulk_activity();
        if (!fs_on_bulk(type, xfer, payload, len) &&
            !inbound_on_bulk(type, xfer, payload, len)) {
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
    g_connected.store(true);
    LINK_LOG("connected: %s", s.peer_name.c_str());
}

void service_on_packet(const uint8_t *pkt, size_t len) {
    LinkLock lock(__func__);
    State &s = state();
    if (!s.connected) {
        return;
    }
    s.rx_packets++;
    reassembler().feed(pkt, len);
}

void service_on_disconnect() {
    LinkLock lock(__func__);
    State &s = state();

    s.connected = false; /* refuse new calls; done() must not restart a transfer */
    g_connected.store(false);
    outbound_reset();
    inbound_reset();

    if (s.rpc) {
        fs_reset();
        s.rpc->disconnect();
        s.total_resets += s.rpc->stats().link_resets;
        s.total_malformed += s.rpc->stats().malformed;
        s.total_timeouts += s.rpc->stats().timeouts;
        s.rpc.reset();
    }
    s.send_packet = nullptr;
    s.tx_room = nullptr;
    s.bulk_activity = nullptr;
    s.wake = nullptr;

    LINK_LOG("disconnected: %s", s.peer_name.c_str());
}

void service_tick() {
    if (!g_connected.load()) {
        return;
    }
    /* The gauge read (I2C) happens before g_mutex, so a slow bus never holds
     * up handlers or pager-initiated calls (P7.35, the IS.8 residual). */
    monitor_params_t mp;
    hw_get_monitor_params(mp);

    LinkLock lock(__func__);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return;
    }

    s.rpc->tick();
    fs_tick(steady_ms());

    const uint32_t now = steady_ms();

    /* Outbound sends with no ACK for the retransmit window: rewind and resend. */
    for (size_t i = 0; i < s.outbound.size();) {
        State::Outbound &o = s.outbound[i];
        if (static_cast<uint32_t>(now - o.rto_ms) >= XFER_RETRANSMIT_MS) {
            o.rto_ms = now;
            if (o.tx->retransmit()) {
                LINK_LOG("send retransmit id=%u at=%u", static_cast<unsigned>(o.xfer),
                         static_cast<unsigned>(o.tx->acked()));
                const XferState st = o.tx->pump();
                if (st == XferState::Done) {
                    finish_outbound(i, true); /* erases index i; do not advance */
                    continue;
                }
                if (st == XferState::Failed) {
                    finish_outbound(i, false); /* erases index i; do not advance */
                    continue;
                }
            }
        }
        ++i;
    }

    /* Outbound sends that saw no ACK progress for their stall window fail. */
    for (size_t i = 0; i < s.outbound.size();) {
        if (static_cast<uint32_t>(now - s.outbound[i].last_ms) > s.outbound[i].stall_ms) {
            LINK_LOG("send stalled id=%u", static_cast<unsigned>(s.outbound[i].xfer));
            finish_outbound(i, false); /* erases index i; do not advance */
        } else {
            ++i;
        }
    }

    /* Inbound receivers with a waiter that saw no DATA for their stall window fail. */
    for (size_t i = 0; i < s.inbound.size(); ++i) {
        State::Inbound &in = s.inbound[i];
        if (in.done && static_cast<uint32_t>(now - in.last_ms) > in.stall_ms) {
            LINK_LOG("accept stalled id=%u", static_cast<unsigned>(in.xfer));
            finish_inbound(i, false); /* entry stays (spent) until close */
        }
    }

    /* Any local write (editor save, link upload, eviction) bumps the
     * generation; tell the phone to drop its listing cache. */
    const uint32_t gen = hw_get_filesystem_generation();
    if (gen != s.last_fs_gen) {
        s.last_fs_gen = gen;
        s.rpc->emit("evt.fs.changed", "{}");
    }

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

uint32_t service_next_tick_ms(uint32_t cap) {
    LinkLock lock(__func__);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return cap;
    }
    /* hw_set_filesystem_dirty() wakes the transport (storage.h generation hook);
     * a moved generation makes the tick due now so evt.fs.changed goes out. */
    if (hw_get_filesystem_generation() != s.last_fs_gen) {
        return 0;
    }
    uint32_t wait = s.rpc->next_wait_ms(cap);
    wait = fs_next_wait_ms(wait);
    const uint32_t now = steady_ms();
    /* The checks service_tick() makes; the stall ones are strict `>`. */
    for (const State::Outbound &o : s.outbound) {
        wait = min_wait_ms(wait, now, o.rto_ms + XFER_RETRANSMIT_MS);
        wait = min_wait_ms(wait, now, o.last_ms + o.stall_ms + 1);
    }
    for (const State::Inbound &in : s.inbound) {
        if (in.done) {
            wait = min_wait_ms(wait, now, in.last_ms + in.stall_ms + 1);
        }
    }
    return wait;
}

bool service_connected() {
    return g_connected.load();
}

std::string service_peer_name() {
    LinkLock lock(__func__);
    State &s = state();
    return s.connected ? s.peer_name : std::string();
}

void service_get_stats(HwLinkStats &out) {
    LinkLock lock(__func__);
    State &s = state();
    out.rx_packets = s.rx_packets;
    out.tx_packets = s.tx_packets;
    out.rx_messages = s.rx_messages;
    out.link_resets = s.total_resets + (s.rpc ? s.rpc->stats().link_resets : 0);
    out.malformed = s.total_malformed + (s.rpc ? s.rpc->stats().malformed : 0);
    out.timeouts = s.total_timeouts + (s.rpc ? s.rpc->stats().timeouts : 0);
    out.rx_dropped = 0; /* the transport fills it */
}

size_t service_packet_size() {
    LinkLock lock(__func__);
    return state().packet_size;
}

void service_set_packet_size(size_t packet_size) {
    LinkLock lock(__func__);
    State &s = state();
    if (!s.connected || packet_size < 2 || packet_size == s.packet_size) {
        return;
    }
    LINK_LOG("packet size %u -> %u", static_cast<unsigned>(s.packet_size),
             static_cast<unsigned>(packet_size));
    s.packet_size = packet_size;
}

void service_bulk_activity() {
    LinkLock lock(__func__);
    note_bulk_activity();
}

void service_rx_window(uint16_t &credits, unsigned &ack_every) {
    LinkLock lock(__func__);
    State &s = state();
    credits = rx_credits_for(s.rx_queue_packets, s.packet_size);
    ack_every = rx_ack_every_for(credits);
}

bool service_call(const std::string &method, const std::string &params_json, uint32_t timeout_ms,
                  Rpc::ResponseFn cb) {
    LinkLock lock(__func__);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return false;
    }
    const bool ok = s.rpc->call(method, params_json, std::move(cb), timeout_ms);
    if (ok) {
        note_new_deadline();
    }
    return ok;
}

bool service_send_bytes(uint16_t xfer, const uint8_t *data, size_t len, uint32_t stall_ms,
                        std::function<void(bool ok)> done) {
    LinkLock lock(__func__);
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
    o.rto_ms = o.last_ms;
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
    note_bulk_activity();
    note_new_deadline();

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

uint16_t service_accept_open(uint32_t max_size) {
    LinkLock lock(__func__);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return 0;
    }
    uint16_t id = s.next_inbound_id;
    size_t tries = 0;
    while (inbound_find(id) >= 0 || fs_owns_xfer(id)) {
        id = (id == 65535) ? 1 : static_cast<uint16_t>(id + 2);
        if (++tries > 32768) return 0;
    }
    s.next_inbound_id = (id == 65535) ? 1 : static_cast<uint16_t>(id + 2);

    State::Inbound in;
    in.xfer = id;
    in.max_size = max_size;
    in.early_bytes = 0;
    in.expecting = false;
    in.failed = false;
    in.crc = 0;
    in.last_ms = steady_ms();
    in.stall_ms = 0;
    s.inbound.push_back(std::move(in));
    LINK_LOG("accept open id=%u max=%u", static_cast<unsigned>(id),
             static_cast<unsigned>(max_size));
    return id;
}

bool service_accept_expect(uint16_t xfer, uint32_t size, uint32_t crc32, uint32_t stall_ms,
                           std::function<void(bool ok, std::string &bytes)> done) {
    LinkLock lock(__func__);
    State &s = state();
    const long idx = inbound_find(xfer);
    if (idx < 0) return false;
    const size_t i = static_cast<size_t>(idx);
    if (s.inbound[i].expecting) return false;

    State::Inbound &in = s.inbound[i];
    in.expecting = true;
    in.done = std::move(done);
    in.crc = crc32;
    in.stall_ms = stall_ms;
    in.last_ms = steady_ms();
    if (in.failed || size > in.max_size || !s.connected || !s.rpc) {
        finish_inbound(i, false);
        return true;
    }
    in.buf.clear();
    in.buf.reserve(size);
    note_bulk_activity();
    note_new_deadline();
    uint16_t credits = 0;
    unsigned ack_every = 0;
    service_rx_window(credits, ack_every);
    in.rx.reset(new XferReceiver(
        size,
        [xfer](uint32_t off, const uint8_t *bytes, size_t n) -> bool {
            const long k = inbound_find(xfer);
            if (k < 0) return false;
            std::string &b = state().inbound[static_cast<size_t>(k)].buf;
            if (off != b.size()) return false;
            b.append(reinterpret_cast<const char *>(bytes), n);
            return true;
        },
        [xfer](const std::string &payload) -> bool {
            State &st = state();
            return st.rpc && st.rpc->send_bulk(MsgType::Ack, xfer, payload);
        },
        credits, ack_every, 0));
    LINK_LOG("accept expect id=%u size=%u early=%u", static_cast<unsigned>(xfer),
             static_cast<unsigned>(size), static_cast<unsigned>(in.early.size()));

    if (in.rx->state() == XferState::Done) { /* zero-length body */
        finish_inbound(i, true);
        return true;
    }
    /* Replay what arrived before we knew the size. Re-find by id each time: a
     * finish leaves the entry in place, but stay defensive. */
    std::vector<std::string> early;
    early.swap(in.early);
    in.early_bytes = 0;
    for (const std::string &p : early) {
        const long k = inbound_find(xfer);
        if (k < 0 || !s.inbound[static_cast<size_t>(k)].done) break;
        inbound_feed(static_cast<size_t>(k), reinterpret_cast<const uint8_t *>(p.data()),
                     p.size());
    }
    return true;
}

void service_accept_close(uint16_t xfer) {
    LinkLock lock(__func__);
    const long idx = inbound_find(xfer);
    if (idx < 0) return;
    State &s = state();
    s.inbound.erase(s.inbound.begin() + idx);
}

bool service_accept_owns(uint16_t xfer) {
    LinkLock lock(__func__);
    return inbound_find(xfer) >= 0;
}

bool service_emit(const std::string &event, const std::string &params_json) {
    LinkLock lock(__func__);
    State &s = state();
    if (!s.connected || !s.rpc) {
        return false;
    }
    return s.rpc->emit(event, params_json.empty() ? "{}" : params_json);
}

int service_subscribe(const std::string &event,
                      std::function<void(const std::string &params_json)> fn) {
    LinkLock lock(__func__);
    Subscribers &subs = subscribers();
    const int token = subs.next_token++;
    subs.subs.emplace_back(token, std::make_pair(event, std::move(fn)));
    return token;
}

void service_unsubscribe(int token) {
    LinkLock lock(__func__);
    Subscribers &subs = subscribers();
    for (size_t i = 0; i < subs.subs.size(); ++i) {
        if (subs.subs[i].first == token) {
            subs.subs.erase(subs.subs.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

}  // namespace pagerlink
