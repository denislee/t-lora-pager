/**
 * @file      link_transport_ble.h
 * @brief     PagerLink GATT transport on the ble_stack-owned server (hardware).
 *
 * Internal to the HAL: hal/ble_stack.cpp drives these hooks. UI code uses
 * hal/link.h.
 */
#pragma once

#ifdef ARDUINO

#include <cstdint>

class NimBLEServer;

namespace pagerlink {
namespace ble {

/** Create the PagerLink service and start the link task (ble_stack, once). */
void build(NimBLEServer *server);

/** Bluetooth turned on / off (ble_stack). Off drops the companion. */
void set_enabled(bool on);

/** A peer disconnected (NimBLE host task). */
void on_disconnect(uint16_t conn_handle);

}  // namespace ble
}  // namespace pagerlink

#endif  // ARDUINO
