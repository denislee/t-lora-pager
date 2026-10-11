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

#include <cstddef>
#include <cstdint>
#include <functional>
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

/*
 * Pager-initiated requests (D→P). Used by apps whose work the phone does for
 * them (weather, chat, notes sync, Telegram — HUB_REMOVAL_PLAN.md §2).
 *
 * Threading: hw_link_call() and hw_link_send_bytes() BLOCK the calling task
 * until the phone answers. Call them from a worker task (OneShotJob, an app's
 * FreeRTOS worker) — never from the LVGL thread, which would freeze the UI,
 * and never from inside a link handler or event callback, which would
 * deadlock the link task that delivers the answer.
 */

// Outcome of hw_link_call(). ok → `json` holds the RES result object. Not ok →
// `code` is the PROTOCOL.md §3.4 code: the phone's ERR code, or the local
// "timeout" / "disconnected" (no phone connected, or the link dropped while
// waiting; HalError::LinkOffline).
struct HwLinkResult {
    bool ok = false;
    std::string json;
    std::string code;
    std::string msg;
};

// Send a request to the phone and wait for its answer. `params_json` is a JSON
// object text or "" (no params). Default timeout 10 s (PROTOCOL.md §3.3).
HwLinkResult hw_link_call(const char *method, const std::string &params_json,
                          uint32_t timeout_ms = 10000);

// Stream `len` bytes to the phone as DATA under transfer `xfer` — the id the
// phone allocated in the RPC that announced the transfer (e.g. chat.audio.begin
// → {xfer}). Blocks until the phone has acknowledged every byte (true), or the
// link drops / nothing is acknowledged for `stall_ms` (false). `data` must stay
// valid until the call returns.
bool hw_link_send_bytes(uint16_t xfer, const uint8_t *data, size_t len,
                        uint32_t stall_ms = 10000);

// Subscribe to a phone event (e.g. "evt.notes.sync"). `fn` receives the event's
// params as JSON object text and runs on the link task: copy what you need and
// return — no LVGL calls, no blocking, no hw_link_call. Returns a token (> 0).
using HwLinkEventFn = std::function<void(const std::string &params_json)>;
int hw_link_subscribe(const char *event, HwLinkEventFn fn);
// Remove a subscription; unknown tokens are ignored. Safe from any task.
void hw_link_unsubscribe(int token);

