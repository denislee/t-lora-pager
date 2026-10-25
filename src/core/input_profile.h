/**
 * @file      input_profile.h
 * @brief     Input-stutter profiling (plan item IS.0).
 *
 * Records where the input path spends time so we can find the source of
 * UI stutter: how long the top-level instance lock is held and by whom,
 * the keyboard reader's own lock wait, key enqueue->dequeue latency in
 * the LVGL keypad read_cb, whole `lv_timer_handler()` passes, and named
 * spans. All recording happens with the instance lock held or on the
 * single LVGL thread, so it adds no locking of its own.
 *
 * Compiled in only when `INPUT_PROFILE` is defined. Enable it with:
 *
 *     pio run -e tlora_pager_profile
 *
 * (that env adds `-D INPUT_PROFILE`). Output goes to the serial monitor
 * with the prefix `[prof]`. When the macro is off every entry point and
 * the `INPUT_PROF_SPAN` / `INPUT_PROF` macros collapse to nothing, so
 * call sites can stay in the tree at zero cost.
 */
#pragma once
#include <stdint.h>
#ifdef INPUT_PROFILE
namespace core { namespace prof {
uint64_t now_us();
// Instance-lock tracker. Called by instanceLockTake() right after the take
// succeeds (wait_us = time spent blocked) and by instanceLockGive() right
// before the give.
void lock_acquired(uint64_t wait_us);
void lock_released();
// Keyboard reader's own lock wait (after IS.1 it takes core::ScopedKbLock).
void kb_lock_wait(uint64_t wait_us);
// Key enqueue->dequeue latency, recorded by the LVGL keypad read_cb.
void key_latency(uint64_t us);
// Snapshot every LVGL timer's last_run right before lv_timer_handler(), so
// frame() can name the timers that ran in a slow (>= 100 ms) frame.
void frame_begin();
// One lv_timer_handler() pass.
void frame(uint64_t us);
// Boot marker (OPTIMIZATION_PHASE6.md G.1): records esp_timer time under a static name; the whole list is printed once, at the first report after the first display flush.
void boot_mark(const char *name);
// Pre-app time (BOOT_TIME_PLAN BT.1): call first thing in setup(). Records esp_clk_rtc_time() (time since the RTC timer reset, i.e. power-on / chip reset; not reset by esp_restart) next to esp_timer time and the reset reason; printed with the boot table.
void boot_pre_app();
// Named span, logged when >= 5 ms.
void span(const char *name, uint64_t us);
// Dump the default group's members (why = short label).
void dump_group(const char *why);
// Called once per LVGL loop; prints a summary every 10 s if anything was recorded.
// The first call also starts the serial input console (device only): one
// command per line on the USB serial port, so a host script can drive the UI
//   k <text>      type <text> (press+release per char); escapes \e ESC,
//                 \n Enter, \t Tab, \b Backspace, \\ backslash
//   r u|d|c [n]   rotary up / down / center click, n times (default 1)
//   b <n> <ms>    burst: n letters, one every <ms> ms
//   a <App>       open an app (System::queueEnterApp)
//   f             mark the filesystem dirty (forces a Journal rescan)
//   g             dump the focus group now
//   s             print the window summary now
void report_tick();
class Span {
public:
    explicit Span(const char *name) : name_(name), t0_(now_us()) {}
    ~Span() { span(name_, now_us() - t0_); }
    Span(const Span &) = delete; Span &operator=(const Span &) = delete;
private:
    const char *name_; uint64_t t0_;
};
}} // namespace core::prof
#define INPUT_PROF_CAT2(a, b) a##b
#define INPUT_PROF_CAT(a, b) INPUT_PROF_CAT2(a, b)
#define INPUT_PROF_SPAN(name) ::core::prof::Span INPUT_PROF_CAT(input_prof_span_, __LINE__)(name)
#define INPUT_PROF(stmt) do { stmt; } while (0)
#else
#define INPUT_PROF_SPAN(name) ((void)0)
#define INPUT_PROF(stmt) ((void)0)
#endif
