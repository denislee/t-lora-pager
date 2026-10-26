/**
 * @file      system_hooks.h
 * @brief     Canonical declarations for cross-module system entry points.
 *
 * Every TU that reaches across module boundaries (`ui_is_fake_sleep`,
 * `menu_show_deferred`, the instance lock, the fake-sleep notifies) goes
 * through this header. Replaces ad-hoc `extern bool ui_is_fake_sleep();` declarations
 * that previously drifted across factory.ino, hal/lvgl_task.cpp, and
 * hal/nfc_task.cpp — a rename used to be a landmine, now it's `grep`-able
 * from one place.
 *
 * Kept intentionally free of LVGL and vendor headers so it's cheap to
 * include from HAL / task TUs that have no UI dependency.
 */
#pragma once

#include "scoped_lock.h"

namespace core {

// One-shot initializer for the instance-lock mutex. Must run before any
// instanceLockTake() call on hardware. The emulator build is a no-op.
void instance_lock_init();

} // namespace core

// Display/power idle flag. `true` while the backlight is off and the UI
// is suspended (tasks skip expensive work, loop() drops CPU freq). Set
// via ui_pause_timers() / ui_resume_timers() in ui_main.cpp.
bool ui_is_fake_sleep();
void ui_pause_timers();
void ui_resume_timers();

// Fake-sleep task-coordination notifies. During fake-sleep the LVGL,
// keyboard and charge tasks block instead of polling; these unblock
// them the moment the state changes so wake/overlay latency stays low
// without a busy poll. All are no-ops on the emulator build.
//   - hw_lvgl_task_notify_wake(): the display is coming back (ui_resume_timers)
//     or an editor switch needs a render cycle (ui_request_editor_switch).
//   - hw_keyboard_task_notify_wake(): the display is coming back
//     (ui_resume_timers); the keyboard task should resume scanning at once so
//     the first keypress after wake isn't delayed.
//   - hw_charge_task_on_fake_sleep_enter(): fake-sleep was entered
//     (ui_pause_timers); the charge task should start watching VBUS.
void hw_lvgl_task_notify_wake();
void hw_keyboard_task_notify_wake();
void hw_charge_task_on_fake_sleep_enter();
//   - hw_ble_kb_task_notify_wake(): the display is coming back
//     (ui_resume_timers); the BLE keepalive task should resume so conn-params
//     can be re-applied after a sleep during which the iOS link may have
//     dropped. No-op on emulator.
void hw_ble_kb_task_notify_wake();

// Vendor-compat aliases for the instance lock. New in-tree code should
// prefer core::ScopedInstanceLock directly.
void ui_lock();
void ui_unlock();

// Fake-sleep wake hook, called by the vendor's fake-sleep toggle. Despite
// the (vendor-fixed) name it returns to the home menu, not the editor.
void ui_request_editor_switch();

// Vendor rotary task: long-press of the center button while the screen is
// on. Defers hw_deep_sleep_wheel_wake() onto the LVGL task. Safe from any task.
void ui_request_wheel_sleep();

// Show the home menu panel (core::System::queueShowMenu) after the current
// LVGL event dispatch unwinds.
// Use this from event callbacks (back buttons, ESC handlers, unlock results)
// so the app is not torn down under the widget that is still dispatching.
void menu_show_deferred();

// True while a queued app switch / menu_show_deferred() has not run yet.
// The keypad and encoder read callbacks hold input while this is set, so keys
// pressed during a transition land on the new screen instead of the old one.
bool ui_transition_pending();

// True while any SSH session (LibSshBackend or loopback stub) is in the
// Connected state. Set/cleared by ui_ssh.cpp; read by factory.ino loop() as a
// guard that prevents auto fake-sleep while a live terminal session is open.
// Safe to call from any task — backed by a volatile bool.
bool ssh_session_is_active();
