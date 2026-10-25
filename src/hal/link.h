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
 *  - BLE GATT on hardware (PROTOCOL.md §1), on the NimBLE server
 *    hal/ble_stack.cpp shares with the HID keyboard. It is up whenever
 *    Bluetooth is on; the phone becomes the companion by subscribing to `tx`.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "result.h"

#ifndef ARDUINO
// Emulator only: open the TCP listener once (env PAGERLINK_PORT, "0" keeps it
// off). On hardware the link has no switch of its own: it follows Bluetooth.
void hw_link_start();
#endif

// True while a companion is connected over the transport.
bool hw_link_connected();

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

// Map a failed hw_link_call() to a HalError plus a UI-ready `err`:
// "disconnected" -> LinkOffline, anything else -> InternalError with
// "phone: <code>: <msg>".
HalError hal_error_from_link(const HwLinkResult &r, std::string &err);

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

/*
 * Phone -> pager bulk into memory (PROTOCOL.md §4: the receiver allocates the
 * transfer id, so the pager's ids are odd). For a request whose answer may be
 * too big for a RES (4096 B): open a receiver, send its id as "xfer" in the
 * request params, and if the phone answers {xfer,size,crc32} instead of an
 * inline result, wait for the bytes. Close on every exit path.
 *
 *   uint16_t x = hw_link_accept_open(64 * 1024);
 *   ... hw_link_call(method, params-with-"xfer":x) ...
 *   ... if the RES is {xfer,size,crc32}: hw_link_accept_wait(x, size, crc, out) ...
 *   hw_link_accept_close(x);
 *
 * DATA that arrives before hw_link_accept_wait() (it can overtake the waiter
 * being woken by the RES) is buffered and replayed. Same threading rules as
 * hw_link_call(): accept_wait blocks.
 */

// Allocate an odd transfer id and install a memory receiver accepting at most
// `max_size` bytes. Returns 0 when no phone is connected or no id is free.
uint16_t hw_link_accept_open(uint32_t max_size);

// Wait until the `size` bytes the phone announced for `xfer` have arrived and
// their CRC-32/ISO-HDLC equals `crc32`; true moves them into `out`. False on a
// size above the open's max, a CRC mismatch, a stall (no DATA for `stall_ms`),
// a disconnect, or an unknown id. Call at most once per open.
bool hw_link_accept_wait(uint16_t xfer, uint32_t size, uint32_t crc32, std::string &out,
                         uint32_t stall_ms = 10000);

// Drop the receiver (and anything it buffered). Unknown ids are ignored.
void hw_link_accept_close(uint16_t xfer);

// Subscribe to a phone event (e.g. "evt.notes.sync"). `fn` receives the event's
// params as JSON object text and runs on the link task: copy what you need and
// return — no LVGL calls, no blocking, no hw_link_call. Returns a token (> 0).
using HwLinkEventFn = std::function<void(const std::string &params_json)>;
int hw_link_subscribe(const char *event, HwLinkEventFn fn);
// Remove a subscription; unknown tokens are ignored. Safe from any task.
void hw_link_unsubscribe(int token);

