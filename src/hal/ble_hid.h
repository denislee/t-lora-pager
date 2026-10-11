/**
 * @file      ble_hid.h
 * @brief     BLE HID keyboard + media keys on the ble_stack-owned server.
 *
 * Ported from the lewisxhe/ESP32-BLE-Keyboard fork onto NimBLEHIDDevice
 * (IOS_COMPANION_PLAN.md Spike S1, decision D1): same report map, report ids,
 * PnP ids and ASCII map, so hosts see the same keyboard. Internal to the HAL;
 * UI code uses the hw_*_ble_kb_* functions in hal/wireless.h.
 *
 * Hardware-only (ARDUINO).
 */
#pragma once

#ifdef ARDUINO

#include <cstdint>

class NimBLEServer;

namespace hal {
namespace ble_hid {

/** Media key bits of the consumer-control report (report id 2). */
enum MediaKey : uint16_t {
    MEDIA_NEXT_TRACK = 1u << 0,
    MEDIA_PREVIOUS_TRACK = 1u << 1,
    MEDIA_STOP = 1u << 2,
    MEDIA_PLAY_PAUSE = 1u << 3,
    MEDIA_MUTE = 1u << 4,
    MEDIA_VOLUME_UP = 1u << 5,
    MEDIA_VOLUME_DOWN = 1u << 6,
};

/** Create the HID, Device Information and Battery services (ble_stack, once). */
void build(NimBLEServer *server);

/** A peer disconnected (NimBLE host task). */
void on_disconnect(uint16_t conn_handle);

/** True while at least one peer is subscribed to a HID input report. */
bool connected();

/** Connection handle of the first keyboard peer, or 0xFFFF when none. */
uint16_t peer_conn_handle();

/** Type `text` (ASCII; unmappable bytes are skipped). No-op when not connected. */
void type(const char *text);

/** Press and release one media key. No-op when not connected. */
void media_key(MediaKey key);

/** Send empty keyboard and media reports (also the iOS keepalive). */
void release_all();

}  // namespace ble_hid
}  // namespace hal

#endif  // ARDUINO
