/**
 * @file      link.h
 * @brief     PagerLink companion link — public HAL API.
 *
 * The pager talks to the iOS companion over the PagerLink protocol
 * (protocol/PROTOCOL.md). This is the only header UI code includes for that
 * feature: it never sees the pagerlink transport, framing or RPC internals.
 *
 * Transports:
 *  - TCP on the emulator (127.0.0.1:7600, PROTOCOL.md §5) so the iOS app can
 *    talk to the SDL build from the Simulator.
 *  - BLE GATT on hardware — NOT wired yet: it is blocked on Spike S1 in
 *    IOS_COMPANION_PLAN.md (HID and the link service must share one NimBLE
 *    server), so on ARDUINO builds hw_link_enable(true) returns false.
 */
#pragma once

#include <cstdint>
#include <string>

// Start/stop the transport; returns the resulting enabled state. On hardware
// this is a no-op that returns false until Spike S1 lands (see above); on the
// emulator it opens the TCP listener (env PAGERLINK_PORT, "0" keeps it off).
bool hw_link_enable(bool en);

// True while the transport is enabled (the listener is up).
bool hw_link_enabled();

// True while a companion is connected over the transport.
bool hw_link_connected();

// Human-readable peer name. "" when not connected; over TCP "tcp:<ip>:<port>".
std::string hw_link_peer_name();

// Cumulative link counters for the diagnostics UI.
struct HwLinkStats {
    uint32_t rx_packets, tx_packets, rx_messages, link_resets, malformed, timeouts;
};

// Copy the current counters into `out`.
void hw_link_get_stats(HwLinkStats &out);
