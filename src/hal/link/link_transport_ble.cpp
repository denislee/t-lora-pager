/**
 * @file      link_transport_ble.cpp
 * @brief     PagerLink BLE GATT transport (PROTOCOL.md §1) + the hw_link_*
 *            API on hardware. The emulator's TCP transport is
 *            link_transport_tcp.cpp.
 *
 * The PagerLink service lives on the server hal/ble_stack.cpp owns, next to
 * HID. A connection becomes the companion when its peer subscribes to `tx`;
 * one companion at a time (a second subscriber is ignored until the first
 * leaves). Packet size P = ATT_MTU − 3 at subscribe time, updated when the
 * MTU exchange completes later (P7.33). While bulk bytes move the link task
 * holds the transfer connection parameters (15–30 ms, latency 0) and restores
 * the idle band BULK_IDLE_MS after the last DATA/ACK.
 *
 * Threading (IOS_COMPANION_PLAN.md §5.2): NimBLE host-task callbacks only copy
 * the event into a queue and return. The `link` task drains it and is the
 * only caller of the service lifecycle (connect / packet / tick /
 * disconnect). It blocks indefinitely while no companion is connected; while
 * one is, it sleeps until the nearest RPC/transfer deadline or the
 * transfer-band restore, at most IDLE_TICK_MS (P7.35). A deadline registered
 * from another task, or a filesystem generation bump, wakes it early. The queue holds a whole credit
 * window (P7.30); a packet that still finds it full is dropped rather than
 * blocking the host task. Outbound packets go the other way through a second
 * queue: the service enqueues them without waiting, and the `link_tx` task
 * notifies them outside the service mutex (P7.32).
 *
 * Security: `rx` requires an encrypted link (bonded Just Works today).
 * PROTOCOL.md §1 also asks for authentication, which needs passkey pairing
 * (Spike S5); until then an unauthenticated encrypted link is accepted.
 */
#include "hal/link.h"

#ifdef ARDUINO

#include "hal/link/link_transport_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#if defined(CONFIG_NIMBLE_CPP_IDF)
#include "os/os_mbuf.h"
#else
#include "nimble/porting/nimble/include/os/os_mbuf.h"
#endif
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <string>

#include "hal/ble_hid.h"
#include "hal/ble_stack.h"
#include "hal/link/link_frame.h"
#include "hal/link/link_log.h"
#include "hal/link/link_service.h"
#include "hal/link/link_uuids.h"
#include "hal/storage.h"
#include "hal/system.h"

namespace pagerlink {
namespace ble {
namespace {

constexpr uint16_t NO_HANDLE = 0xFFFF;
/** Largest packet: ATT_MTU − 3 (ble_stack.h caps the MTU, so a write can't be larger). */
constexpr size_t MAX_PACKET = hal::ble::ATT_MTU - 3;
static_assert(MAX_PACKET == TCP_PACKET, "BLE and TCP use the same packet size");
/** Packets kept free for REQ/RES/ACK traffic interleaved with a bulk window. */
constexpr UBaseType_t RX_SLACK = 32;
/** Event queue depth: a full implicit credit window (INITIAL_CREDITS max-size messages,
 * PROTOCOL.md §4) plus slack, so a phone that respects credits never overflows it (P7.30).
 * ~76 KB of PSRAM. */
constexpr UBaseType_t QUEUE_DEPTH =
    INITIAL_CREDITS * packets_per_message(MAX_MESSAGE, MAX_PACKET) + RX_SLACK;
constexpr UBaseType_t QUEUE_DEPTH_INTERNAL = 16;
/** Longest link_task sleep while a companion is up and nothing is pending: the
 * battery evt cadence. Deadlines (RPC timeouts, retransmit RTO, stalls, the
 * BULK_IDLE_MS band restore) cut it short (P7.35). */
constexpr uint32_t IDLE_TICK_MS = 10000;
/** Minimum gap between two service_tick() runs, so a deadline that stays due
 * can't spin the task. */
constexpr uint32_t MIN_TICK_GAP_MS = 20;
/* P7.37: kept at 8 KB until the profile build's `[prof] stack free: link` (P7.49)
 * is read during a 256 KB fs.write + tg bulk reply; shrink only from that number. */
constexpr uint32_t TASK_STACK = 8192;
/** Outbound packets queued for the tx task: a whole credit window of max-size
 * messages plus slack for RPC traffic (~84 KB of PSRAM, P7.32). */
constexpr UBaseType_t TX_QUEUE_DEPTH =
    INITIAL_CREDITS * packets_per_message(MAX_MESSAGE, MAX_PACKET) + 2 * RX_SLACK;
constexpr UBaseType_t TX_QUEUE_DEPTH_INTERNAL =
    2 * packets_per_message(MAX_MESSAGE, MAX_PACKET) + 8;
constexpr uint32_t TX_TASK_STACK = 4096;
/** notify() fails while the host is out of mbufs; the tx task polls this often. */
constexpr uint32_t NOTIFY_RETRY_MS = 2;
/** msys blocks link_tx leaves free for HID key reports and incoming traffic: a
 * bulk window never takes the pool's last blocks (P7.31). */
constexpr int HID_MBUF_RESERVE = 4;
/** Give up on a packet after this long without radio space: twice the supervision
 * timeout, clamped (the link is dead by then and the disconnect is on its way). */
constexpr uint32_t NOTIFY_STALL_MIN_MS = 2000;
constexpr uint32_t NOTIFY_STALL_MAX_MS = 12000;

enum class Kind : uint8_t { Subscribe, Unsubscribe, Disconnect, Packet, Drop, Mtu, Wake };

struct Event {
    Kind kind;
    uint16_t conn;
    uint16_t len;
    uint8_t data[MAX_PACKET];  // packet bytes, the peer address for Subscribe, or the MTU (u16) for Mtu
};

/** One outbound packet. `epoch` ties it to a companion session so a resubscribe on
 * the same handle never receives the previous session's packets. */
struct TxPacket {
    uint32_t epoch;
    uint16_t conn;
    uint16_t len;
    uint8_t data[MAX_PACKET];
};

NimBLEServer *g_server = nullptr;
NimBLECharacteristic *g_tx = nullptr;
QueueHandle_t g_queue = nullptr;
SemaphoreHandle_t g_post_lock = nullptr;
TaskHandle_t g_task = nullptr;
QueueHandle_t g_txq = nullptr;
SemaphoreHandle_t g_tx_lock = nullptr;
TaskHandle_t g_tx_task = nullptr;
UBaseType_t g_txq_depth = 0;

std::atomic<bool> g_bt_on{false};
/** Companion connection handle; written only by the link task. */
std::atomic<uint16_t> g_companion{NO_HANDLE};
std::atomic<uint32_t> g_dropped{0};
std::atomic<uint32_t> g_dropped_total{0};
/** Bumped by companion_up; packets of an older session are discarded. */
std::atomic<uint32_t> g_epoch{0};
/** Stall budget for the current companion (companion_up). */
std::atomic<uint32_t> g_stall_ms{NOTIFY_STALL_MIN_MS};
std::atomic<uint32_t> g_tx_dropped{0};
/** Set by the service's bulk_activity hook (any task, service lock held);
 * consumed by the link task, which owns the connection-parameter band (P7.33). */
std::atomic<bool> g_bulk_seen{false};
std::atomic<uint32_t> g_bulk_ms{0};
/** Band the companion runs in; link task only. */
ConnBand g_band = ConnBand::Idle;
/** Queue depth in use; 0 until create_queue() picks a path. */
UBaseType_t g_queue_depth = 0;
/** A Kind::Wake is queued and not yet consumed (at most one in flight). */
std::atomic<bool> g_wake_queued{false};

/**
 * Queue an event. Producers are the NimBLE host task and set_enabled();
 * g_post_lock serialises them over the one static staging Event (~250 B, kept
 * off the host task's stack). Control events wait up to 50 ms for queue space;
 * packets never wait (a dropped packet costs one message, which the transfer
 * engine's rewind/retransmit recovers, P7.29), so HID reports and GATT events
 * are never stalled behind link_task.
 */
void post(Kind kind, uint16_t conn, const uint8_t *data, size_t len) {
    if (!g_queue || !g_post_lock) {
        return;
    }
    static Event ev;
    xSemaphoreTake(g_post_lock, portMAX_DELAY);
    ev.kind = kind;
    ev.conn = conn;
    ev.len = static_cast<uint16_t>(len > MAX_PACKET ? MAX_PACKET : len);
    if (data && ev.len) {
        std::memcpy(ev.data, data, ev.len);
    }
    /* Packets must never block the NimBLE host task. */
    const TickType_t wait = (kind == Kind::Packet) ? 0 : pdMS_TO_TICKS(50);
    if (xQueueSend(g_queue, &ev, wait) != pdTRUE) {
        g_dropped.fetch_add(1);
        g_dropped_total.fetch_add(1);
    }
    xSemaphoreGive(g_post_lock);
}

/**
 * Make link_task re-evaluate its wait (P7.35): a deadline was registered from
 * another task (TransportOps::wake, service lock held) or the filesystem
 * generation moved (storage hook, any task). Lock-free and never blocks: one
 * Wake in flight at most; a full queue means link_task is about to loop anyway.
 */
void wake_link() {
    if (!g_queue || g_wake_queued.exchange(true)) {
        return;
    }
    static const Event wake_ev = {Kind::Wake, NO_HANDLE, 0, {}};
    if (xQueueSend(g_queue, &wake_ev, 0) != pdTRUE) {
        g_wake_queued.store(false);
    }
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *chr, NimBLEConnInfo &info) override {
        const NimBLEAttValue v = chr->getValue();
        if (v.size() > MAX_PACKET) {
            g_dropped.fetch_add(1);
            g_dropped_total.fetch_add(1);
            return;
        }
        // Empty writes pass through: they reset the reassembler (PROTOCOL.md §2).
        post(Kind::Packet, info.getConnHandle(), v.data(), v.size());
    }
};

class TxCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t sub_value) override {
        if (sub_value & 0x0001) {
            const std::string addr = info.getAddress().toString();
            post(Kind::Subscribe, info.getConnHandle(),
                 reinterpret_cast<const uint8_t *>(addr.data()), addr.size());
        } else {
            post(Kind::Unsubscribe, info.getConnHandle(), nullptr, 0);
        }
    }
};

RxCallbacks g_rx_callbacks;
TxCallbacks g_tx_callbacks;

/**
 * Queue one packet for the tx task. Called by the service with its mutex held,
 * so it never waits: a full queue fails the send (the service checks tx_room()
 * first, so that only happens past a credit window, P7.32).
 */
bool send_packet(uint32_t epoch, uint16_t conn, const uint8_t *pkt, size_t len) {
    if (!g_txq || len > MAX_PACKET || g_epoch.load() != epoch) {
        return false;
    }
    static TxPacket staging;  // ~250 B, off the caller's stack
    xSemaphoreTake(g_tx_lock, portMAX_DELAY);
    staging.epoch = epoch;
    staging.conn = conn;
    staging.len = static_cast<uint16_t>(len);
    std::memcpy(staging.data, pkt, len);
    const bool ok = xQueueSend(g_txq, &staging, 0) == pdTRUE;
    xSemaphoreGive(g_tx_lock);
    if (!ok) {
        g_tx_dropped.fetch_add(1);
    }
    return ok;
}

bool tx_room(size_t packets) {
    return g_txq && uxQueueSpacesAvailable(g_txq) >= packets;
}

/** Discard every queued packet (after a stall: the link is not draining). */
void tx_flush() {
    static TxPacket junk;
    uint32_t n = 0;
    while (xQueueReceive(g_txq, &junk, 0) == pdTRUE) {
        ++n;
    }
    g_tx_dropped.fetch_add(n);
}

/**
 * Drains g_txq with notify(), outside the service mutex. notify() fails while
 * the host is out of mbufs; NimBLE has no "space freed" event (its NOTIFY_TX
 * event fires synchronously inside notify with the same code), so poll every
 * NOTIFY_RETRY_MS up to the stall budget. It also waits while fewer than
 * HID_MBUF_RESERVE msys blocks are free, so HID key reports always find one
 * (P7.31).
 */
void tx_task(void *) {
    static TxPacket p;
    for (;;) {
        if (xQueueReceive(g_txq, &p, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint32_t t0 = millis();
        for (;;) {
            if (g_epoch.load() != p.epoch || g_companion.load() != p.conn) {
                break;  // the session ended; drop its leftovers
            }
            /* Leave HID_MBUF_RESERVE blocks for key reports (P7.31). */
            if (os_msys_num_free() >= HID_MBUF_RESERVE &&
                g_tx->notify(p.data, p.len, p.conn)) {
                break;
            }
            if (millis() - t0 >= g_stall_ms.load()) {
                LINK_LOG("[link] notify stalled %u ms; dropping the tx queue",
                         static_cast<unsigned>(g_stall_ms.load()));
                g_tx_dropped.fetch_add(1);
                tx_flush();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_MS));
        }
    }
}

/**
 * Runs on the link task. The GAP requests here and in update_conn_band() are
 * issued from this task rather than from the NimBLE host callbacks: those only
 * queue an event (§5.2), and the link task calls NimBLE outside g_mutex, so no
 * GAP call waits on the host lock while holding the service lock.
 */
void companion_up(uint16_t conn, const std::string &addr) {
    /* Before the MTU exchange (a CCCD restore can beat it) this is 23; the
     * Kind::Mtu event raises P later (P7.33). */
    const uint16_t mtu = g_server->getPeerMTU(conn);
    const size_t packet = packet_size_for_mtu(mtu, hal::ble::ATT_MTU);
    const uint32_t epoch = g_epoch.fetch_add(1) + 1;
    /* Supervision timeout is in 10 ms units. */
    uint32_t stall = 2u * 10u * g_server->getPeerInfoByHandle(conn).getConnTimeout();
    if (stall < NOTIFY_STALL_MIN_MS) stall = NOTIFY_STALL_MIN_MS;
    if (stall > NOTIFY_STALL_MAX_MS) stall = NOTIFY_STALL_MAX_MS;
    g_stall_ms.store(stall);

    TransportOps ops;
    ops.send_packet = [epoch, conn](const uint8_t *pkt, size_t len) {
        return send_packet(epoch, conn, pkt, len);
    };
    ops.tx_room = [](size_t packets) { return tx_room(packets); };
    ops.bulk_activity = [] {
        g_bulk_ms.store(millis());
        g_bulk_seen.store(true);
    };
    ops.wake = [] { wake_link(); };
    ops.packet_size = packet;
    ops.rx_queue_packets =
        g_queue_depth > 2 * RX_SLACK ? g_queue_depth - RX_SLACK : g_queue_depth / 2;
    ops.peer_name = "ble:" + addr;
    g_companion.store(conn);
    g_band = ConnBand::Idle;
    g_bulk_seen.store(false);
    /* DLE 251 + 2M PHY (requests; the peer may refuse, P7.33). */
    hal::ble::request_fast_link(conn);
    /* A keyboard connection already gets the idle band from the HID keepalive
     * task; give a companion-only peer the same one (P7.35). */
    const bool keyboard = hal::ble_hid::connected() && hal::ble_hid::peer_conn_handle() == conn;
    if (!keyboard) {
        hal::ble::apply_idle_conn_params(conn);
    }
    LINK_LOG("[link] companion %s up, mtu=%u rxq=%u txq=%u stall=%ums%s", ops.peer_name.c_str(),
             static_cast<unsigned>(mtu), static_cast<unsigned>(ops.rx_queue_packets),
             static_cast<unsigned>(g_txq_depth), static_cast<unsigned>(stall),
             keyboard ? "" : " (idle params)");
    service_on_connect(ops);
}

/**
 * Hold the transfer band while bulk bytes move and drop back to idle
 * BULK_IDLE_MS after the last DATA/ACK (P7.33). Link task only; link_wait_ms()
 * includes the restore deadline, so idle returns BULK_IDLE_MS (+ MIN_TICK_GAP_MS
 * at worst) after the last bulk message.
 */
void update_conn_band() {
    const uint16_t conn = g_companion.load();
    if (conn == NO_HANDLE) {
        return;
    }
    const bool activity = g_bulk_seen.exchange(false);
    const ConnBand next = next_conn_band(g_band, activity, millis(), g_bulk_ms.load());
    if (next == g_band) {
        return;
    }
    g_band = next;
    if (next == ConnBand::Transfer) {
        LINK_LOG("[link] transfer conn params (15-30 ms, latency 0)");
        hal::ble::apply_transfer_conn_params(conn);
    } else {
        LINK_LOG("[link] idle conn params");
        hal::ble::end_transfer_conn_params(conn, true);
    }
}

/** `peer_gone`: the connection itself dropped, so don't restore its idle band. */
void companion_down(const char *why, bool peer_gone = false) {
    const uint16_t conn = g_companion.load();
    if (conn == NO_HANDLE) {
        return;
    }
    LINK_LOG("[link] companion down (%s)", why);
    g_companion.store(NO_HANDLE);
    if (g_band == ConnBand::Transfer) {
        g_band = ConnBand::Idle;
        hal::ble::end_transfer_conn_params(conn, !peer_gone);
    }
    service_on_disconnect();
}

void handle(const Event &ev) {
    const uint16_t companion = g_companion.load();
    switch (ev.kind) {
    case Kind::Subscribe:
        // iOS keeps the connection up for the system keyboard after the app
        // goes away, so a fresh subscribe on the companion's own handle is a
        // new app session: restart the link (seq = 0) instead of ignoring it.
        if (companion == ev.conn) {
            companion_down("resubscribed");
            companion_up(ev.conn, std::string(reinterpret_cast<const char *>(ev.data), ev.len));
        } else if (companion == NO_HANDLE && g_bt_on.load()) {
            companion_up(ev.conn, std::string(reinterpret_cast<const char *>(ev.data), ev.len));
        } else {
            LINK_LOG("[link] ignoring tx subscribe from handle %u", static_cast<unsigned>(ev.conn));
        }
        break;
    case Kind::Unsubscribe:
        if (ev.conn == companion) {
            companion_down("unsubscribed");
        }
        break;
    case Kind::Disconnect:
        if (ev.conn == companion) {
            companion_down("disconnected", true);
        }
        break;
    case Kind::Drop:
        companion_down("link disabled");
        break;
    case Kind::Packet:
        if (ev.conn == companion) {
            service_on_packet(ev.data, ev.len);
        }
        break;
    case Kind::Wake:
        break; /* consumed in link_task; the loop re-evaluates the wait */
    case Kind::Mtu:
        // An MTU exchange after the subscribe (P7.33); before it, companion_up
        // reads the current MTU itself.
        if (ev.conn == companion && ev.len == sizeof(uint16_t)) {
            uint16_t mtu = 0;
            std::memcpy(&mtu, ev.data, sizeof(mtu));
            service_set_packet_size(packet_size_for_mtu(mtu, hal::ble::ATT_MTU));
        }
        break;
    }
}

/**
 * How long link_task may sleep while a companion is up (P7.35): the nearest
 * service deadline (RPC timeout, retransmit RTO, stalls, a pending
 * evt.fs.changed), the transfer-band restore, and IDLE_TICK_MS since the last
 * tick (measured from that tick, so steady traffic can't starve the battery evt).
 */
uint32_t link_wait_ms(uint32_t last_tick) {
    const uint32_t now = millis();
    uint32_t wait = service_next_tick_ms(IDLE_TICK_MS);
    wait = min_wait_ms(wait, now, last_tick + IDLE_TICK_MS);
    if (g_band == ConnBand::Transfer) {
        wait = min_wait_ms(wait, now, g_bulk_ms.load() + BULK_IDLE_MS);
    }
    /* A transfer started on another task sets g_bulk_seen and wakes us
     * (TransportOps::wake); update_conn_band() after the receive handles it. */
    return wait;
}

void link_task(void *) {
    // Event is ~250 B; static so the task stack only carries handler depth.
    static Event ev;
    uint32_t last_tick = millis();
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (g_companion.load() != NO_HANDLE) {
            uint32_t wait_ms = link_wait_ms(last_tick);
            if (wait_ms == 0 && millis() - last_tick >= MIN_TICK_GAP_MS) {
                last_tick = millis();
                service_tick();
                update_conn_band();
                wait_ms = link_wait_ms(last_tick);
            }
            if (wait_ms < MIN_TICK_GAP_MS) {
                wait_ms = MIN_TICK_GAP_MS;
            }
            wait = pdMS_TO_TICKS(wait_ms);
        }
        if (xQueueReceive(g_queue, &ev, wait) == pdTRUE) {
            if (ev.kind == Kind::Wake) {
                g_wake_queued.store(false);
            } else {
                handle(ev);
            }
        }
        update_conn_band();
        const uint32_t dropped = g_dropped.exchange(0);
        if (dropped) {
            LINK_LOG("[link] dropped %u events (queue full or oversize), %u total",
                     static_cast<unsigned>(dropped),
                     static_cast<unsigned>(g_dropped_total.load()));
        }
        const uint32_t tx_dropped = g_tx_dropped.exchange(0);
        if (tx_dropped) {
            LINK_LOG("[link] dropped %u tx packets (queue full or stalled)",
                     static_cast<unsigned>(tx_dropped));
        }
    }
}

/** Queue storage in PSRAM when available (§5.2: PSRAM for link buffers). */
QueueHandle_t create_queue() {
    static StaticQueue_t qs;
    void *storage = heap_caps_malloc(QUEUE_DEPTH * sizeof(Event), MALLOC_CAP_SPIRAM);
    if (storage) {
        g_queue_depth = QUEUE_DEPTH;
        return xQueueCreateStatic(QUEUE_DEPTH, sizeof(Event), static_cast<uint8_t *>(storage), &qs);
    }
    g_queue_depth = QUEUE_DEPTH_INTERNAL;
    return xQueueCreate(QUEUE_DEPTH_INTERNAL, sizeof(Event));
}

/** Tx queue storage in PSRAM when available, like the rx queue. */
QueueHandle_t create_tx_queue() {
    static StaticQueue_t qs;
    void *storage = heap_caps_malloc(TX_QUEUE_DEPTH * sizeof(TxPacket), MALLOC_CAP_SPIRAM);
    if (storage) {
        g_txq_depth = TX_QUEUE_DEPTH;
        return xQueueCreateStatic(TX_QUEUE_DEPTH, sizeof(TxPacket),
                                   static_cast<uint8_t *>(storage), &qs);
    }
    g_txq_depth = TX_QUEUE_DEPTH_INTERNAL;
    return xQueueCreate(TX_QUEUE_DEPTH_INTERNAL, sizeof(TxPacket));
}

}  // namespace

void build(NimBLEServer *server) {
    g_server = server;
    NimBLEService *svc = server->createService(SERVICE_UUID);

    NimBLECharacteristic *rx = svc->createCharacteristic(
        RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC,
        MAX_PACKET);
    rx->setCallbacks(&g_rx_callbacks);

    g_tx = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY, MAX_PACKET);
    g_tx->setCallbacks(&g_tx_callbacks);

    NimBLECharacteristic *info = svc->createCharacteristic(INFO_UUID, NIMBLE_PROPERTY::READ);
    char json[128];
    snprintf(json, sizeof(json), "{\"proto\":%u,\"fw\":\"%s\",\"board\":\"%s\",\"l2cap_psm\":0}",
             static_cast<unsigned>(PROTO_VERSION), hw_get_firmware_hash_string(), board_tag());
    info->setValue(std::string(json));

    g_post_lock = xSemaphoreCreateMutex();
    g_tx_lock = xSemaphoreCreateMutex();
    g_queue = create_queue();
    g_txq = create_tx_queue();
    if (!g_queue || !g_post_lock || !g_txq || !g_tx_lock) {
        log_e("[link] event queue alloc failed; PagerLink over BLE disabled");
        return;
    }
    // Low priority (§5.2); core 0 next to the NimBLE host.
    if (xTaskCreatePinnedToCore(link_task, "link", TASK_STACK, nullptr, tskIDLE_PRIORITY + 1, &g_task,
                                0) != pdPASS) {
        log_e("[link] task create failed; PagerLink over BLE disabled");
        g_task = nullptr;
    }
    // Same priority and core as link_task; it only sleeps or notifies (P7.32).
    if (g_task && xTaskCreatePinnedToCore(tx_task, "link_tx", TX_TASK_STACK, nullptr,
                                          tskIDLE_PRIORITY + 1, &g_tx_task, 0) != pdPASS) {
        log_e("[link] tx task create failed; PagerLink over BLE disabled");
        vTaskDelete(g_task);
        g_task = nullptr;
        g_tx_task = nullptr;
    }
    if (g_task) {
        hw_set_filesystem_generation_hook(wake_link); /* evt.fs.changed, P7.35 */
    }
}

void set_enabled(bool on) {
    g_bt_on.store(on);
    if (!on) {
        post(Kind::Drop, NO_HANDLE, nullptr, 0);
    }
}

void on_disconnect(uint16_t conn_handle) { post(Kind::Disconnect, conn_handle, nullptr, 0); }

void on_mtu_change(uint16_t conn_handle, uint16_t mtu) {
    post(Kind::Mtu, conn_handle, reinterpret_cast<const uint8_t *>(&mtu), sizeof(mtu));
}

}  // namespace ble
}  // namespace pagerlink

bool hw_link_connected() { return pagerlink::service_connected(); }

#endif  // ARDUINO
