/**
 * @file      ble_stack.h
 * @brief     Owner of the NimBLE stack: one server shared by HID and PagerLink.
 *
 * IOS_COMPANION_PLAN.md §5.1. ble_stack initialises NimBLEDevice once (the
 * first time Bluetooth is turned on) and never deinits it: NimBLE leaks on
 * every init/deinit cycle. "Bluetooth off" stops advertising and drops every
 * peer instead.
 *
 * The one server carries, in this order (existing keyboard bonds cached the
 * HID handles, so they stay first):
 *   1. Device Information + HID + Battery (hal/ble_hid.cpp)
 *   2. PagerLink (hal/link/link_transport_ble.cpp)
 *
 * Server callbacks are owned here and fanned out to both modules per
 * connection handle. A connection is a keyboard when its peer subscribes to
 * the HID input report, and a companion when it subscribes to PagerLink `tx`;
 * one iPhone can be both on the same connection.
 *
 * Hardware-only (ARDUINO); the emulator has no BLE.
 */
#pragma once

#ifdef ARDUINO

#include <cstdint>
#include <string>

namespace hal {
namespace ble {

/** Device name advertised and used by existing bonds; do not change. */
constexpr const char *DEVICE_NAME = "lilygo-pager";

/** Init the stack on first use, then start advertising. True on success. */
bool enable();

/** Stop advertising and disconnect every peer. The stack stays initialised. */
void disable();

/** True between enable() and disable(). */
bool enabled();

/** Disconnect one peer. */
void disconnect(uint16_t conn_handle);

/** Peer address of `conn_handle` ("" when not connected). */
std::string peer_address(uint16_t conn_handle);

/** Number of connected peers (any role). */
uint8_t connected_count();

/**
 * Apply the iOS-compliant idle connection parameters to `conn_handle`
 * (30–50 ms, latency 10, 6.72 s timeout; see ble_stack.cpp).
 */
void apply_idle_conn_params(uint16_t conn_handle);

}  // namespace ble
}  // namespace hal

#endif  // ARDUINO
