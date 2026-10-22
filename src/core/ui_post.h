/**
 * @file      ui_post.h
 * @brief     Thread-safe "run this on the LVGL thread" queue (P7.47).
 *
 * Any task may post a closure here. `core::System::loop()` drains the queue on
 * the LVGL thread, under the instance lock, immediately after
 * `lv_timer_handler()` has returned. `ui_post()` never runs the closure
 * inline — that makes it safe to call from LVGL callbacks too — and it wakes
 * the LVGL task (`hw_lvgl_task_notify_wake`) so the drain doesn't have to wait
 * for the next display tick.
 *
 * The queue is bounded at 32 pending items: a post to a full queue is dropped
 * and returns false, so a runaway producer cannot grow the heap without bound.
 *
 * A closure may run after the app that posted it has exited, so it must
 * re-check its app's state (null out its widgets in onStop, test for null in
 * the closure) rather than assume the page is still alive.
 */
#pragma once

#include <functional>

namespace core {

// Enqueue `fn` to run on the LVGL thread. Callable from any task. Returns
// false when the closure was dropped (empty `fn`, or 32 already pending).
bool ui_post(std::function<void()> fn);

// Drain the queue and run every pending closure, without holding the queue
// lock while they run. LVGL thread only (`core::System::loop()`).
void ui_post_drain();

} // namespace core
