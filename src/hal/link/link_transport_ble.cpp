/**
 * @file      link_transport_ble.cpp
 * @brief     PagerLink BLE GATT transport (PROTOCOL.md §1) + the hw_link_*
 *            API on hardware. The emulator's TCP transport is
 *            link_transport_tcp.cpp.
 *
 * The PagerLink service lives on the server hal/ble_stack.cpp owns, next to
 * HID. A connection becomes the companion when its peer subscribes to `tx`;
 * one companion at a time (a second subscriber is ignored until the first
 * leaves). Packet size P = ATT_MTU − 3 at subscribe time.
 *
 * Threading (IOS_COMPANION_PLAN.md §5.2): NimBLE host-task callbacks only copy
 * the event into a queue and return. The `link` task drains it and is the
 * only caller of the service lifecycle (connect / packet / tick /
 * disconnect). It blocks indefinitely while no companion is connected, and
 * pumps service_tick() at 1 Hz while one is. The queue holds a whole credit
 * window (P7.30); a packet that still finds it full is dropped rather than
 * blocking the host task.
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
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <string>

#include "hal/ble_stack.h"
#include "hal/link/link_frame.h"
#include "hal/link/link_service.h"
#include "hal/link/link_uuids.h"
#include "hal/system.h"

#define LINK_LOG(...) log_i(__VA_ARGS__)

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
constexpr uint32_t TICK_MS = 1000;
constexpr uint32_t TASK_STACK = 8192;
/** notify() fails while the controller is out of mbufs; retry this long. */
constexpr int NOTIFY_RETRIES = 100;
constexpr uint32_t NOTIFY_RETRY_MS = 2;

enum class Kind : uint8_t { Subscribe, Unsubscribe, Disconnect, Packet, Drop };

struct Event {
    Kind kind;
    uint16_t conn;
    uint16_t len;
    uint8_t data[MAX_PACKET];  // packet bytes, or the peer address for Subscribe
};

NimBLEServer *g_server = nullptr;
NimBLECharacteristic *g_tx = nullptr;
QueueHandle_t g_queue = nullptr;
SemaphoreHandle_t g_post_lock = nullptr;
TaskHandle_t g_task = nullptr;

std::atomic<bool> g_bt_on{false};
std::atomic<bool> g_user_off{false};
/** Companion connection handle; written only by the link task. */
std::atomic<uint16_t> g_companion{NO_HANDLE};
std::atomic<uint32_t> g_dropped{0};
std::atomic<uint32_t> g_dropped_total{0};
/** Queue depth in use; 0 until create_queue() picks a path. */
UBaseType_t g_queue_depth = 0;

bool accepting() { return g_bt_on.load() && !g_user_off.load(); }

/**
 * Queue an event. Producers are the NimBLE host task and hw_link_enable();
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

bool send_packet(uint16_t conn, const uint8_t *pkt, size_t len) {
    for (int i = 0; i < NOTIFY_RETRIES; ++i) {
        if (g_companion.load() != conn) {
            return false;  // the phone left mid-send
        }
        if (g_tx->notify(pkt, len, conn)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_MS));
    }
    LINK_LOG("[link] notify failed; giving up on packet");
    return false;
}

void companion_up(uint16_t conn, const std::string &addr) {
    uint16_t mtu = g_server->getPeerMTU(conn);
    if (mtu < 23) {
        mtu = 23;
    }
    if (mtu > hal::ble::ATT_MTU) {
        mtu = hal::ble::ATT_MTU;
    }
    TransportOps ops;
    ops.send_packet = [conn](const uint8_t *pkt, size_t len) { return send_packet(conn, pkt, len); };
    ops.packet_size = static_cast<size_t>(mtu) - 3;
    ops.rx_queue_packets =
        g_queue_depth > 2 * RX_SLACK ? g_queue_depth - RX_SLACK : g_queue_depth / 2;
    ops.peer_name = "ble:" + addr;
    g_companion.store(conn);
    LINK_LOG("[link] companion %s up, mtu=%u rxq=%u", ops.peer_name.c_str(),
             static_cast<unsigned>(mtu), static_cast<unsigned>(ops.rx_queue_packets));
    service_on_connect(ops);
}

void companion_down(const char *why) {
    if (g_companion.load() == NO_HANDLE) {
        return;
    }
    LINK_LOG("[link] companion down (%s)", why);
    g_companion.store(NO_HANDLE);
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
        } else if (companion == NO_HANDLE && accepting()) {
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
            companion_down("disconnected");
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
    }
}

void link_task(void *) {
    // Event is ~250 B; static so the task stack only carries handler depth.
    static Event ev;
    uint32_t last_tick = millis();
    for (;;) {
        const bool active = g_companion.load() != NO_HANDLE;
        const TickType_t wait = active ? pdMS_TO_TICKS(TICK_MS) : portMAX_DELAY;
        if (xQueueReceive(g_queue, &ev, wait) == pdTRUE) {
            handle(ev);
        }
        if (g_companion.load() != NO_HANDLE && millis() - last_tick >= TICK_MS) {
            last_tick = millis();
            service_tick();
        }
        const uint32_t dropped = g_dropped.exchange(0);
        if (dropped) {
            LINK_LOG("[link] dropped %u events (queue full or oversize), %u total",
                     static_cast<unsigned>(dropped),
                     static_cast<unsigned>(g_dropped_total.load()));
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

const char *board_tag() {
#ifdef ARDUINO_T_LORA_PAGER
    return "pager";
#elif defined(ARDUINO_T_WATCH_S3)
    return "twatch_s3";
#elif defined(ARDUINO_T_WATCH_S3_ULTRA)
    return "twatch_ultra";
#else
    return "unknown";
#endif
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
    g_queue = create_queue();
    if (!g_queue || !g_post_lock) {
        log_e("[link] event queue alloc failed; PagerLink over BLE disabled");
        return;
    }
    // Low priority (§5.2); core 0 next to the NimBLE host.
    if (xTaskCreatePinnedToCore(link_task, "link", TASK_STACK, nullptr, tskIDLE_PRIORITY + 1, &g_task,
                                0) != pdPASS) {
        log_e("[link] task create failed; PagerLink over BLE disabled");
        g_task = nullptr;
    }
}

void set_enabled(bool on) {
    g_bt_on.store(on);
    if (!on) {
        post(Kind::Drop, NO_HANDLE, nullptr, 0);
    }
}

void on_disconnect(uint16_t conn_handle) { post(Kind::Disconnect, conn_handle, nullptr, 0); }

}  // namespace ble
}  // namespace pagerlink

using namespace pagerlink::ble;

bool hw_link_enable(bool en) {
    g_user_off.store(!en);
    if (!en) {
        post(Kind::Drop, NO_HANDLE, nullptr, 0);
    }
    return hw_link_enabled();
}

bool hw_link_enabled() { return g_task != nullptr && accepting(); }

bool hw_link_connected() { return pagerlink::service_connected(); }

std::string hw_link_peer_name() { return pagerlink::service_peer_name(); }

void hw_link_get_stats(HwLinkStats &out) {
    pagerlink::service_get_stats(out);
    out.rx_dropped = g_dropped_total.load();
}

#endif  // ARDUINO
