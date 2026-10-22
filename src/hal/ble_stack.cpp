/**
 * @file      ble_stack.cpp
 * @brief     NimBLE stack owner: init once, one server, callback fan-out.
 *
 * See ble_stack.h and IOS_COMPANION_PLAN.md §5.1.
 */
#include "ble_stack.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>

#include <atomic>

#include "ble_hid.h"
#include "link/link_transport_ble.h"
#include "link/link_uuids.h"

namespace hal {
namespace ble {
namespace {

NimBLEServer *g_server = nullptr;
bool g_enabled = false;
/** Connection holding the transfer band (P7.33), or NO_CONN. Written by the
 * PagerLink link task, read by the HID keepalive task. */
constexpr uint16_t NO_CONN = 0xFFFF;
std::atomic<uint16_t> g_transfer_conn{NO_CONN};
/** Supervision timeout shared by both bands (10 ms units): 6.72 s. Keeping it
 * equal means PagerLink's notify stall budget (2 x timeout) stays valid. */
constexpr uint16_t CONN_TIMEOUT = 0x2A0;

/** Appearance: generic keyboard (Bluetooth Assigned Numbers 0x03C1). */
constexpr uint16_t APPEARANCE_KEYBOARD = 0x03C1;
/** HID service, 16-bit UUID. */
constexpr uint16_t HID_SERVICE_UUID16 = 0x1812;
/**
 * GATT layout revision. Bump whenever services or characteristics are added,
 * removed or reordered. 1 = HID + PagerLink (Spike S1); the BleKeyboard fork's
 * HID-only layout was implicitly 0.
 */
constexpr uint32_t GATT_REV = 1;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info, int reason) override {
        const uint16_t h = info.getConnHandle();
        log_i("[ble] disconnect handle=%u reason=0x%x", h, reason);
        hal::ble_hid::on_disconnect(h);
        pagerlink::ble::on_disconnect(h);
        // NimBLE stops advertising on connect; bring it back so the next peer
        // (or this one, reconnecting) can find us.
        if (g_enabled) {
            NimBLEDevice::startAdvertising();
        }
    }

    void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override {
        log_i("[ble] mtu handle=%u mtu=%u", info.getConnHandle(), mtu);
        pagerlink::ble::on_mtu_change(info.getConnHandle(), mtu);
    }

    void onAuthenticationComplete(NimBLEConnInfo &info) override {
        if (!info.isEncrypted()) {
            log_w("[ble] encryption failed; disconnecting %s",
                  info.getAddress().toString().c_str());
            NimBLEDevice::getServer()->disconnect(info.getConnHandle());
        }
    }
};

/**
 * Advertising data: flags, appearance = keyboard, HID UUID, full name (fits
 * in 31 bytes). Scan response: the 128-bit PagerLink service UUID, so an iOS
 * app can find the pager with a service-filtered scan (PROTOCOL.md §1).
 */
void configure_advertising() {
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->reset();

    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_BREDR_UNSUP | BLE_HS_ADV_F_DISC_GEN);
    data.setAppearance(APPEARANCE_KEYBOARD);
    data.addServiceUUID(NimBLEUUID(HID_SERVICE_UUID16));
    data.setName(DEVICE_NAME, true);

    NimBLEAdvertisementData scan;
    scan.addServiceUUID(NimBLEUUID(pagerlink::SERVICE_UUID));

    adv->setAdvertisementData(data);
    adv->setScanResponseData(scan);
    adv->enableScanResponse(true);
    adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);
    // Preferred connection interval hint, unchanged from the BleKeyboard
    // fork: 160 × 1.25 ms = 200 ms … 320 × 1.25 ms = 400 ms.
    adv->setPreferredParams(160, 320);
}

/**
 * iOS caches a bonded peripheral's GATT database and only rediscovers it on a
 * Service Changed indication (NimBLE builds without GATT caching, so there is
 * no Database Hash). Phones bonded to an older layout would never see the
 * PagerLink service. When the layout revision differs from the one recorded
 * in NVS, indicate "everything changed": with no peer connected at boot,
 * NimBLE persists the flag in each bonded peer's CCCD record and sends the
 * indication after that peer reconnects and encrypts.
 */
void announce_layout_change() {
    Preferences prefs;
    if (!prefs.begin("ble", false)) {
        return;
    }
    const uint32_t stored = prefs.getUInt("gatt_rev", 0);
    if (stored != GATT_REV) {
        log_i("[ble] GATT layout %u -> %u; flagging Service Changed for bonded peers",
              static_cast<unsigned>(stored), static_cast<unsigned>(GATT_REV));
        g_server->sendServiceChangedIndication();
        prefs.putUInt("gatt_rev", GATT_REV);
    }
    prefs.end();
}

/** First enable: init NimBLE, create the shared server and its services. */
bool init_stack() {
    if (!NimBLEDevice::init(DEVICE_NAME)) {
        log_e("[ble] NimBLEDevice::init failed");
        return false;
    }
    // PB.12/Lever4: 0 dBm instead of the +3 dBm default; the pager is used
    // within a metre of the phone.
    NimBLEDevice::setPower(0);
    // Bonding + LE Secure Connections, Just Works (no IO). Bonding keeps iOS
    // from re-prompting "Allow" on every reconnect. Passkey pairing
    // (DISPLAY_ONLY) is Spike S5.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    // ATT_MTU 247 → PagerLink packet size 244, the same P as over TCP.
    NimBLEDevice::setMTU(ATT_MTU);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks(), true);

    // Order matters: HID first keeps the attribute handles that existing
    // keyboard bonds cached; PagerLink is appended after it.
    hal::ble_hid::build(g_server);
    pagerlink::ble::build(g_server);

    g_server->start();
    announce_layout_change();
    configure_advertising();
    return true;
}

}  // namespace

bool enable() {
    if (!g_server && !init_stack()) {
        return false;
    }
    g_enabled = true;
    pagerlink::ble::set_enabled(true);
    if (!NimBLEDevice::getAdvertising()->isAdvertising() &&
        g_server->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        if (!NimBLEDevice::startAdvertising()) {
            log_e("[ble] advertising failed to start");
        }
    }
    return true;
}

void disable() {
    if (!g_server) {
        return;
    }
    g_enabled = false;
    pagerlink::ble::set_enabled(false);
    NimBLEDevice::stopAdvertising();
    for (uint16_t h : g_server->getPeerDevices()) {
        g_server->disconnect(h);
    }
    log_d("[ble] disabled");
}

bool enabled() { return g_enabled; }

void disconnect(uint16_t conn_handle) {
    if (g_server) {
        g_server->disconnect(conn_handle);
    }
}

std::string peer_address(uint16_t conn_handle) {
    if (!g_server) {
        return "";
    }
    NimBLEConnInfo info = g_server->getPeerInfoByHandle(conn_handle);
    if (info.getConnHandle() != conn_handle) {
        return "";
    }
    return info.getAddress().toString();
}

uint8_t connected_count() { return g_server ? g_server->getConnectedCount() : 0; }

void apply_idle_conn_params(uint16_t conn_handle) {
    // iOS enforces Apple's Accessory Design Guidelines for BLE HID: outside
    // this band the iPhone silently tears the link down after a few minutes.
    // 0x18 = 30 ms, 0x28 = 50 ms, latency 10 (answer every 11th event, ~10×
    // fewer idle radio wakes), timeout 0x2A0 = 6.72 s. Check: 50 ms × 11 =
    // 550 ms ≤ 2 s; 6.72 s ≥ 3 × 550 ms.
    if (g_server && g_transfer_conn.load() != conn_handle) {
        g_server->updateConnParams(conn_handle, 0x18, 0x28, 10, CONN_TIMEOUT);
    }
}

void apply_transfer_conn_params(uint16_t conn_handle) {
    // IOS_COMPANION_PLAN.md §5.1 item 7: 12 = 15 ms, 24 = 30 ms, latency 0, so
    // the phone's writes go out on every event instead of waiting up to 10
    // skipped ones. Still inside Apple's guidelines: min >= 15 ms, max >= min +
    // 15 ms, timeout 6.72 s >= 3 x 30 ms.
    if (!g_server) {
        return;
    }
    g_transfer_conn.store(conn_handle);
    g_server->updateConnParams(conn_handle, 12, 24, 0, CONN_TIMEOUT);
}

void end_transfer_conn_params(uint16_t conn_handle, bool restore) {
    uint16_t expected = conn_handle;
    if (!g_transfer_conn.compare_exchange_strong(expected, NO_CONN)) {
        return;
    }
    if (restore) {
        apply_idle_conn_params(conn_handle);
    }
}

void request_fast_link(uint16_t conn_handle) {
    if (!g_server) {
        return;
    }
    // 251 = the largest LL payload (BT 4.2 DLE): one 244 B PagerLink packet + the
    // 4 B L2CAP and 3 B ATT headers fits a single radio packet.
    g_server->setDataLen(conn_handle, 251);
    g_server->updatePhy(conn_handle, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
}

}  // namespace ble
}  // namespace hal

#endif  // ARDUINO
