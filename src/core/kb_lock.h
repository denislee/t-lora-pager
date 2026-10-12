/**
 * @file      kb_lock.h
 * @brief     RAII wrapper around a dedicated keyboard mutex (kb_lock.cpp).
 *
 * Serialises every access to `instance.kb`, the TCA8418 keyboard driver:
 * key reads (`getKey`), the begin/end lifecycle, backlight brightness, the
 * read callback, the notify-task registration, and the vendor lightSleep()/
 * sleep() paths that internally call `kb.end()` / `kb.begin()`.
 *
 * Replaces the instance mutex for the keyboard reader (INPUT_STUTTER_PLAN.md
 * IS.1) so key reads no longer wait for the LVGL frame: the LVGL task holds
 * the instance mutex for a whole frame, and any long frame (app build, save,
 * flush) would otherwise stall the drain loop. With a keyboard-only mutex the
 * reader keeps reading and queuing keys during a long frame, and the LVGL
 * keypad read_cb applies them in one burst once the frame ends
 * (continue_reading) — same as the rotary path.
 *
 * The underlying mutex is RECURSIVE: hw_enable_keyboard() holds ScopedKbLock
 * and then calls hw_set_kb_backlight(), which takes it again.
 *
 * Lock order: a caller may hold `core::ScopedInstanceLock` and then take
 * `ScopedKbLock`; never the reverse. Nothing under `ScopedKbLock` may take
 * the instance lock.
 *
 * The Wire (I2C) bus itself stays safe for other users (PMIC, RTC, sensors):
 * arduino-esp32 builds Wire with its per-transaction HAL lock
 * (CONFIG_DISABLE_HAL_LOCKS unset), so a keyboard transaction cannot corrupt
 * a concurrent PMIC/RTC/sensor transaction on the same bus.
 *
 * Declared here without pulling in FreeRTOS so the header is cheap to include
 * from HAL and UI TUs; the implementation lives in kb_lock.cpp.
 */
#pragma once

namespace core {

// Create the keyboard mutex. Call once from setup() before any task starts.
// Idempotent; safe to call again.
void kb_lock_init();

class ScopedKbLock {
public:
    ScopedKbLock();
    ~ScopedKbLock();
    ScopedKbLock(const ScopedKbLock &) = delete;
    ScopedKbLock &operator=(const ScopedKbLock &) = delete;
    ScopedKbLock(ScopedKbLock &&) = delete;
    ScopedKbLock &operator=(ScopedKbLock &&) = delete;
};

} // namespace core
