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
#include <memory>
#include <mutex>

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
};

State &state() {
    static State s;
    return s;
}

/** Milliseconds from a monotonic clock, wrapping at 2^32 (Rpc's ClockFn). */
uint32_t steady_ms() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
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
    s.rpc->on_bulk(fs_on_bulk);
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

    if (s.rpc) {
        fs_reset();
        s.rpc->disconnect();
        s.total_resets += s.rpc->stats().link_resets;
        s.total_malformed += s.rpc->stats().malformed;
        s.total_timeouts += s.rpc->stats().timeouts;
        s.rpc.reset();
    }
    s.send_packet = nullptr;
    s.connected = false;

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

}  // namespace pagerlink
