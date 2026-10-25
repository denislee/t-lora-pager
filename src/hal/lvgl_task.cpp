/**
 * @file      lvgl_task.cpp
 * @brief     Dedicated LVGL handler task.
 *
 * Running `lv_timer_handler()` from the Arduino `loop()` meant every
 * tick paid the cost of whatever else shared that loop (vendor
 * instance.loop, NFC polling, CPU scaling reads). Here we give LVGL
 * its own core-1 task so render and input-processing cadence is
 * determined by LVGL itself, not by an unrelated loop body.
 *
 * The task calls `core::System::getInstance().loop()` alongside LVGL
 * because both drive UI-side state and must see each tick together.
 */
#include "lvgl_task.h"

#ifdef ARDUINO

#include <lvgl.h>
#include <LV_Helper.h>

#include "../core/input_profile.h"
#include "../core/scoped_lock.h"
#include "../core/system.h"
#include "task_util.h"
#include "display.h"

namespace {

// Priority sits above the Arduino loopTask (1) but well below WiFi/BLE system
// tasks so we don't block radio work. Running at configMAX_PRIORITIES - 2 meant
// a long app-side operation (journal scan / AES decrypt) could starve IDLE1 on
// core 1, which the task watchdog catches as a panic → reboot. 8 is generous
// for UI responsiveness without the starvation hazard.
constexpr UBaseType_t kTaskPriority = 8;
constexpr BaseType_t  kTaskCore     = 1;
// Cap for lv_timer_handler()'s returned deadline while the display is on.
// lv_timer_handler() returns the real next deadline — small values when
// animations or timers are running, and large values (≥ LV_DEF_REFR_PERIOD =
// 33 ms) when the screen is fully static. We honor that deadline instead of
// forcing a 60 Hz wake-up on every iteration; a fully static screen now
// sleeps up to 200 ms between LVGL ticks, cutting ~60 unnecessary mutex
// acquisitions/s to near zero. Active animations and timers are unaffected
// because they return deadlines well below 33 ms.
// Input immediacy is preserved: keyboard_task.cpp and rotary_task.cpp both
// call hw_lvgl_task_notify_wake() from their enqueue_event() so key/scroll
// events wake this task immediately rather than waiting up to 200 ms. The
// task blocks on ulTaskNotifyTake() with this deadline instead of
// vTaskDelay(), so it actually observes those notifications, and reads the
// encoder/keypad straight away when input is what woke it instead of waiting
// for the next indev read timer.
constexpr uint32_t    kMaxTickMs    = 200;
// FFat reads plus mbedTLS AES-CBC decrypt plus nested LVGL event dispatch
// (e.g. menu rebuild from a click handler) easily cleared 6KB on 8KB stacks.
constexpr uint32_t    kStackBytes   = 16384;

TaskHandle_t s_task = nullptr;

// BT.11: brightness waiting for the first flushed frame (-1 = none pending).
int16_t s_boot_backlight = -1;
volatile bool s_first_flush = false;

void first_flush_cb(lv_event_t *) { s_first_flush = true; }

// Under the instance lock, right after lv_timer_handler(). Skipped during fake
// sleep: the wake path restores the backlight itself, and the next pass here
// then corrects it to the user's level.
void apply_boot_backlight()
{
    if (s_boot_backlight < 0 || !s_first_flush || ui_is_fake_sleep()) return;
    if (lv_display_t *disp = lv_display_get_default()) {
        lv_display_remove_event_cb_with_user_data(disp, first_flush_cb, nullptr);
    }
    hw_set_disp_backlight((uint8_t)s_boot_backlight);
    s_boot_backlight = -1;
    INPUT_PROF(core::prof::boot_mark("backlight on"));
}

void lvgl_task_fn(void *)
{
    for (;;) {
        // Nothing to render while the display is off. Block until a wake /
        // editor-switch notification (hw_lvgl_task_notify_wake() from
        // ui_resume_timers()) arrives instead of spinning; the hal::kTaskParkMs
        // timeout is only a safety net so a missed notify still recovers.
        if (hal::park_if_fake_sleep()) continue;

        uint32_t next;
        {
            core::ScopedInstanceLock lock;
#ifdef INPUT_PROFILE
            core::prof::frame_begin();
            uint64_t f0 = core::prof::now_us();
#endif
            next = lv_timer_handler();
            INPUT_PROF(core::prof::frame(core::prof::now_us() - f0));
            apply_boot_backlight();
            core::System::getInstance().loop();
        }

        if (next > kMaxTickMs) next = kMaxTickMs;
        if (next == 0)         next = 1;
        // P6.6: vTaskDelay ignored the input wake from keyboard_task/rotary_task,
        // so a keypress waited for the next indev read timer. Block on the notify
        // instead and, when an input woke us, read the encoder/keypad immediately.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next)) > 0 && !ui_is_fake_sleep()) {
            core::ScopedInstanceLock lock;
            if (lv_indev_t *enc = lv_get_encoder_indev())  lv_indev_read(enc);
            if (lv_indev_t *kb  = lv_get_keyboard_indev()) lv_indev_read(kb);
        }
    }
}

}  // namespace

void hw_lvgl_task_notify_wake() { hal::notify_task(s_task); }

void hw_lvgl_task_set_boot_backlight(uint8_t level) { s_boot_backlight = level; }

void hw_lvgl_task_start()
{
    if (s_task) return;
    if (s_boot_backlight >= 0) {
        lv_display_t *disp = lv_display_get_default();
        if (disp) {
            lv_display_add_event_cb(disp, first_flush_cb, LV_EVENT_FLUSH_FINISH, nullptr);
        } else {
            s_first_flush = true;   // no display to wait for
        }
    }
    hal::start_pinned_task(lvgl_task_fn, "lvgl", kStackBytes,
                           kTaskPriority, kTaskCore, &s_task, "lvgl_task");
}

#else  // !ARDUINO

void hw_lvgl_task_start() {}
void hw_lvgl_task_notify_wake() {}
void hw_lvgl_task_set_boot_backlight(uint8_t) {}

#endif  // ARDUINO
