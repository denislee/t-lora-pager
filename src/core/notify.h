/**
 * @file      notify.h
 * @brief     Process-wide notification bus. Apps post structured notifications
 *            here; a subscriber on the LVGL thread renders them as banners on
 *            `lv_layer_top()`.
 *
 * Threading:
 *  - `post()` is callable from any task (Telegram bg poll, UI handlers). It
 *    takes a mutex briefly and enqueues a copy.
 *  - `pump()` runs from `core::System::loop()` on the LVGL thread, drains the
 *    queue, and renders each banner — the instance lock is already held on
 *    hardware.
 *  - Do NOT call `post()` from an ISR: `Notification` owns `std::string`.
 *    Hand off via a FreeRTOS queue to a task and post from there.
 *
 * Gating:
 *  - The bus itself does not filter on user preferences. Posters decide
 *    whether to fire (see Telegram's `notif_vib` / `notif_toast` prefs).
 */
#pragma once

#include <cstdint>
#include <string>

namespace core {
namespace notify {

enum class Severity : uint8_t {
    Info,
    Error,
};

struct Notification {
    // Short headline. Rendered bold if non-empty.
    std::string title;
    // Body text. Rendered on a second line. May be empty.
    std::string body;
    // Optional LV_SYMBOL_* glyph prepended to the title. nullptr = none.
    const char *icon = nullptr;
    // Controls banner color: Info = accent teal, Error = red.
    Severity severity = Severity::Info;
    // Auto-dismiss timer in ms. 0 means sticky — stays until evicted by newer
    // banners (the renderer caps the visible stack).
    uint32_t duration_ms = 3000;
    // If true, the default renderer fires `hw_feedback()` once when the
    // banner appears. Centralizes the "ding + toast" pattern so posters
    // don't each reach for the haptic themselves.
    bool haptic = false;
    // Assigned by `post()`: the banner's identity for its dismiss timer.
    uint32_t id = 0;
};

// Enqueue a notification. Thread-safe.
void post(Notification n);

// Drains the post queue into the banner renderer. Call from the LVGL thread
// — `core::System::loop()` is the canonical driver. No-op until
// install_default_renderer().
void pump();

// Installs the default toast renderer. Call once from `System::init()` after
// the main screen is built. Idempotent.
void install_default_renderer();

} // namespace notify
} // namespace core
