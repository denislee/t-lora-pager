/**
 * @file      notify.h
 * @brief     Process-wide notification bus. Apps post structured notifications
 *            here; a subscriber on the LVGL thread renders them as banners on
 *            `lv_layer_top()`.
 *
 * Threading:
 *  - `post()` is callable from any task (Telegram bg poll, radio task, UI
 *    handlers). It takes a mutex briefly and enqueues a copy.
 *  - `pump()` runs from `core::System::loop()` on the LVGL thread, drains the
 *    queue, and calls subscribers. Renderers can touch LVGL directly from
 *    their callback — the instance lock is already held on hardware.
 *  - Do NOT call `post()` from an ISR: `Notification` owns `std::string`.
 *    Hand off via a FreeRTOS queue to a task and post from there.
 *
 * Gating:
 *  - The bus itself does not filter on user preferences. Posters decide
 *    whether to fire (see Telegram's `notif_vib` / `notif_toast` prefs).
 *    Subscribers decide whether to render; the default renderer always does.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace core {
namespace notify {

enum class Severity : uint8_t {
    Info,
    Success,
    Warning,
    Error,
};

struct Notification {
    // Short headline. Rendered bold if non-empty.
    std::string title;
    // Body text. Rendered on a second line. May be empty.
    std::string body;
    // Optional LV_SYMBOL_* glyph prepended to the title. nullptr = none.
    const char *icon = nullptr;
    // Controls banner color. Info = accent teal; Success/Warning/Error use
    // the matching palette.
    Severity severity = Severity::Info;
    // Auto-dismiss timer in ms. 0 means sticky — stays until evicted by newer
    // banners (the renderer caps the visible stack).
    uint32_t duration_ms = 3000;
    // If true, the default renderer fires `hw_feedback()` once when the
    // banner appears. Centralizes the "ding + toast" pattern so posters
    // don't each reach for the haptic themselves.
    bool haptic = false;
    // Assigned by `post()` (also returned as its result); the banner's identity.
    uint32_t id = 0;
};

// Enqueue a notification. Returns the assigned id (>0). Thread-safe.
uint32_t post(Notification n);

// Subscribers are invoked on the LVGL thread from `pump()`. Multiple may
// coexist (e.g. default renderer + a status-bar badge counter).
using Subscriber = std::function<void(const Notification &)>;
void subscribe(Subscriber s);

// Drains the post queue and invokes subscribers. Call from the LVGL thread
// — `core::System::loop()` is the canonical driver.
void pump();

// Installs the default toast renderer. Call once from `System::init()` after
// the main screen is built. Idempotent.
void install_default_renderer();

} // namespace notify
} // namespace core
