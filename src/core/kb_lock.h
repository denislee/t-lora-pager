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
 * The Wire (I2C) bus is NOT fully safe for other users on its own.
 * arduino-esp32's HAL lock (CONFIG_DISABLE_HAL_LOCKS unset) covers each
 * transaction, but a read's bytes are then taken from Wire's shared rxBuffer
 * with no lock held. A kb_reader poll that lands between another task's
 * requestFrom() and its read() leaves that read short, and readBytes() then
 * waits out its timeout. On the pager this stalled the battery-gauge read
 * for 1 s. So any I2C READ that can run concurrently with the reader must
 * also hold this lock (hw_get_monitor_params does). Writes are safe without it.
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
