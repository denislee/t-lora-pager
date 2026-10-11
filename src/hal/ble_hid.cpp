/**
 * @file      ble_hid.cpp
 * @brief     BLE HID keyboard + media keys on NimBLEHIDDevice (Spike S1).
 *
 * Report map, report ids, PnP ids and the ASCII → usage table are carried
 * over byte for byte from the lewisxhe/ESP32-BLE-Keyboard fork this replaces,
 * so a bonded host sees the same device.
 *
 * "Connected" means a peer is subscribed to a HID input report — not merely
 * connected — so a companion-only connection (PagerLink, no keyboard) does
 * not light up the keyboard UI or the keepalive.
 */
#include "ble_hid.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

#include <atomic>

namespace hal {
namespace ble_hid {
namespace {

constexpr uint8_t KEYBOARD_ID = 0x01;
constexpr uint8_t MEDIA_KEYS_ID = 0x02;

/** Gap between consecutive reports; iOS drops keys sent back to back. */
constexpr uint32_t REPORT_GAP_MS = 7;

// clang-format off
const uint8_t REPORT_MAP[] = {
    0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,        // USAGE (Keyboard)
    0xA1, 0x01,        // COLLECTION (Application)
    // ------------------------------------------------- Keyboard
    0x85, KEYBOARD_ID, //   REPORT_ID (1)
    0x05, 0x07,        //   USAGE_PAGE (Keyboard/Keypad)
    0x19, 0xE0,        //   USAGE_MINIMUM (0xE0)
    0x29, 0xE7,        //   USAGE_MAXIMUM (0xE7)
    0x15, 0x00,        //   LOGICAL_MINIMUM (0)
    0x25, 0x01,        //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,        //   REPORT_SIZE (1)
    0x95, 0x08,        //   REPORT_COUNT (8)
    0x81, 0x02,        //   INPUT (Data,Var,Abs) ; modifier bits
    0x95, 0x01,        //   REPORT_COUNT (1)
    0x75, 0x08,        //   REPORT_SIZE (8)
    0x81, 0x01,        //   INPUT (Const) ; reserved byte
    0x95, 0x05,        //   REPORT_COUNT (5)
    0x75, 0x01,        //   REPORT_SIZE (1)
    0x05, 0x08,        //   USAGE_PAGE (LEDs)
    0x19, 0x01,        //   USAGE_MINIMUM (Num Lock)
    0x29, 0x05,        //   USAGE_MAXIMUM (Kana)
    0x91, 0x02,        //   OUTPUT (Data,Var,Abs) ; LED bits
    0x95, 0x01,        //   REPORT_COUNT (1)
    0x75, 0x03,        //   REPORT_SIZE (3)
    0x91, 0x01,        //   OUTPUT (Const) ; padding
    0x95, 0x06,        //   REPORT_COUNT (6)
    0x75, 0x08,        //   REPORT_SIZE (8)
    0x15, 0x00,        //   LOGICAL_MINIMUM (0)
    0x25, 0x65,        //   LOGICAL_MAXIMUM (0x65)
    0x05, 0x07,        //   USAGE_PAGE (Keyboard/Keypad)
    0x19, 0x00,        //   USAGE_MINIMUM (0)
    0x29, 0x65,        //   USAGE_MAXIMUM (0x65)
    0x81, 0x00,        //   INPUT (Data,Array,Abs) ; 6 key slots
    0xC0,              // END_COLLECTION
    // ------------------------------------------------- Media keys
    0x05, 0x0C,        // USAGE_PAGE (Consumer)
    0x09, 0x01,        // USAGE (Consumer Control)
    0xA1, 0x01,        // COLLECTION (Application)
    0x85, MEDIA_KEYS_ID, //   REPORT_ID (2)
    0x05, 0x0C,        //   USAGE_PAGE (Consumer)
    0x15, 0x00,        //   LOGICAL_MINIMUM (0)
    0x25, 0x01,        //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,        //   REPORT_SIZE (1)
    0x95, 0x10,        //   REPORT_COUNT (16)
    0x09, 0xB5,        //   USAGE (Scan Next Track)     ; bit 0
    0x09, 0xB6,        //   USAGE (Scan Previous Track) ; bit 1
    0x09, 0xB7,        //   USAGE (Stop)                ; bit 2
    0x09, 0xCD,        //   USAGE (Play/Pause)          ; bit 3
    0x09, 0xE2,        //   USAGE (Mute)                ; bit 4
    0x09, 0xE9,        //   USAGE (Volume Increment)    ; bit 5
    0x09, 0xEA,        //   USAGE (Volume Decrement)    ; bit 6
    0x0A, 0x23, 0x02,  //   USAGE (WWW Home)            ; bit 7
    0x0A, 0x94, 0x01,  //   USAGE (My Computer)         ; bit 8
    0x0A, 0x92, 0x01,  //   USAGE (Calculator)          ; bit 9
    0x0A, 0x2A, 0x02,  //   USAGE (WWW Favorites)       ; bit 10
    0x0A, 0x21, 0x02,  //   USAGE (WWW Search)          ; bit 11
    0x0A, 0x26, 0x02,  //   USAGE (WWW Stop)            ; bit 12
    0x0A, 0x24, 0x02,  //   USAGE (WWW Back)            ; bit 13
    0x0A, 0x83, 0x01,  //   USAGE (Media Select)        ; bit 14
    0x0A, 0x8A, 0x01,  //   USAGE (Mail)                ; bit 15
    0x81, 0x02,        //   INPUT (Data,Var,Abs)
    0xC0,              // END_COLLECTION
};

/** ASCII → keyboard usage; bit 7 set = needs left shift. 0 = unmapped. */
constexpr uint8_t SHIFT = 0x80;
const uint8_t ASCII_MAP[] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    0x2A,          // BS  Backspace
    0x2B,          // TAB Tab
    0x28,          // LF  Enter
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x2C,          // ' '
    0x1E | SHIFT,  // !
    0x34 | SHIFT,  // "
    0x20 | SHIFT,  // #
    0x21 | SHIFT,  // $
    0x22 | SHIFT,  // %
    0x24 | SHIFT,  // &
    0x34,          // '
    0x26 | SHIFT,  // (
    0x27 | SHIFT,  // )
    0x25 | SHIFT,  // *
    0x2E | SHIFT,  // +
    0x36,          // ,
    0x2D,          // -
    0x37,          // .
    0x38,          // /
    0x27,          // 0
    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,  // 1..9
    0x33 | SHIFT,  // :
    0x33,          // ;
    0x36 | SHIFT,  // <
    0x2E,          // =
    0x37 | SHIFT,  // >
    0x38 | SHIFT,  // ?
    0x1F | SHIFT,  // @
    0x04 | SHIFT, 0x05 | SHIFT, 0x06 | SHIFT, 0x07 | SHIFT, 0x08 | SHIFT,  // A..E
    0x09 | SHIFT, 0x0A | SHIFT, 0x0B | SHIFT, 0x0C | SHIFT, 0x0D | SHIFT,  // F..J
    0x0E | SHIFT, 0x0F | SHIFT, 0x10 | SHIFT, 0x11 | SHIFT, 0x12 | SHIFT,  // K..O
    0x13 | SHIFT, 0x14 | SHIFT, 0x15 | SHIFT, 0x16 | SHIFT, 0x17 | SHIFT,  // P..T
    0x18 | SHIFT, 0x19 | SHIFT, 0x1A | SHIFT, 0x1B | SHIFT, 0x1C | SHIFT,  // U..Y
    0x1D | SHIFT,  // Z
    0x2F,          // [
    0x31,          // backslash
    0x30,          // ]
    0x23 | SHIFT,  // ^
    0x2D | SHIFT,  // _
    0x35,          // `
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,  // a..j
    0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,  // k..t
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,                          // u..z
    0x2F | SHIFT,  // {
    0x31 | SHIFT,  // |
    0x30 | SHIFT,  // }
    0x35 | SHIFT,  // ~
    0,             // DEL
};
// clang-format on
static_assert(sizeof(ASCII_MAP) == 128, "ASCII_MAP must cover 0..127");

struct KeyReport {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
};

NimBLEHIDDevice *g_hid = nullptr;
NimBLECharacteristic *g_input_kb = nullptr;
NimBLECharacteristic *g_input_media = nullptr;

/**
 * Up to CONFIG_BT_NIMBLE_MAX_CONNECTIONS keyboard peers. Written on the NimBLE
 * host task (subscribe / disconnect), read from the UI and keepalive tasks.
 */
constexpr size_t MAX_PEERS = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
constexpr uint16_t NO_HANDLE = 0xFFFF;
std::atomic<uint16_t> g_kb_peers[MAX_PEERS];

void peers_reset() {
    for (auto &p : g_kb_peers) {
        p.store(NO_HANDLE);
    }
}

void peer_add(uint16_t h) {
    for (auto &p : g_kb_peers) {
        if (p.load() == h) {
            return;
        }
    }
    for (auto &p : g_kb_peers) {
        uint16_t expected = NO_HANDLE;
        if (p.compare_exchange_strong(expected, h)) {
            return;
        }
    }
}

void peer_remove(uint16_t h) {
    for (auto &p : g_kb_peers) {
        uint16_t expected = h;
        p.compare_exchange_strong(expected, NO_HANDLE);
    }
}

class InputCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t sub_value) override {
        // Fires on a CCCD write and when NimBLE restores a bonded peer's CCCD
        // after encryption. sub_value bit 0 = notify.
        if (sub_value & 0x0001) {
            peer_add(info.getConnHandle());
        }
        // An unsubscribe on one input report does not drop the keyboard: a
        // host may keep the other report; disconnect clears the peer.
    }
};

InputCallbacks g_input_callbacks;

/**
 * Notify `chr` to each keyboard peer. NimBLE's handle-less notify() would
 * reach every connected peer, subscribed or not — including a companion-only
 * connection — so address the keyboard peers explicitly.
 */
void send_report(NimBLECharacteristic *chr, const uint8_t *data, size_t len) {
    if (!chr || !connected()) {
        return;
    }
    for (auto &p : g_kb_peers) {
        const uint16_t h = p.load();
        if (h != NO_HANDLE) {
            chr->notify(data, len, h);
        }
    }
    delay(REPORT_GAP_MS);
}

void send_keyboard(const KeyReport &r) {
    send_report(g_input_kb, reinterpret_cast<const uint8_t *>(&r), sizeof(r));
}

void send_media(uint16_t bits) {
    const uint8_t r[2] = {static_cast<uint8_t>(bits & 0xFF), static_cast<uint8_t>(bits >> 8)};
    send_report(g_input_media, r, sizeof(r));
}

}  // namespace

void build(NimBLEServer *server) {
    peers_reset();
    g_hid = new NimBLEHIDDevice(server);
    g_input_kb = g_hid->getInputReport(KEYBOARD_ID);
    g_hid->getOutputReport(KEYBOARD_ID);  // LED report; host writes are ignored
    g_input_media = g_hid->getInputReport(MEDIA_KEYS_ID);
    g_input_kb->setCallbacks(&g_input_callbacks);
    g_input_media->setCallbacks(&g_input_callbacks);

    // Same PnP ids as the BleKeyboard fork (USB-IF vendor source, Apple
    // keyboard VID/PID) so hosts keep their keyboard layout choice.
    g_hid->setManufacturer("Espressif");
    g_hid->setPnp(0x02, 0x05AC, 0x820A, 0x0210);
    g_hid->setHidInfo(0x00, 0x01);
    g_hid->setReportMap(const_cast<uint8_t *>(REPORT_MAP), sizeof(REPORT_MAP));
    g_hid->setBatteryLevel(100);
}

void on_disconnect(uint16_t conn_handle) { peer_remove(conn_handle); }

bool connected() { return peer_conn_handle() != NO_HANDLE; }

uint16_t peer_conn_handle() {
    for (auto &p : g_kb_peers) {
        const uint16_t h = p.load();
        if (h != NO_HANDLE) {
            return h;
        }
    }
    return NO_HANDLE;
}

void type(const char *text) {
    if (!text || !connected()) {
        return;
    }
    for (const char *p = text; *p; ++p) {
        const uint8_t c = static_cast<uint8_t>(*p);
        if (c == '\r' || c >= 128) {
            continue;
        }
        const uint8_t m = ASCII_MAP[c];
        if (!m) {
            continue;
        }
        KeyReport down{};
        down.modifiers = (m & SHIFT) ? 0x02 : 0x00;  // left shift
        down.keys[0] = m & 0x7F;
        send_keyboard(down);
        send_keyboard(KeyReport{});
    }
}

void media_key(MediaKey key) {
    if (!connected()) {
        return;
    }
    send_media(static_cast<uint16_t>(key));
    send_media(0);
}

void release_all() {
    send_keyboard(KeyReport{});
    send_media(0);
}

}  // namespace ble_hid
}  // namespace hal

#endif  // ARDUINO
