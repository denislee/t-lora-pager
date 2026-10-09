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

#include <Arduino.h>
#include <lvgl.h>
#include <LV_Helper.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../core/scoped_lock.h"
#include "../core/system.h"
#include "../core/system_hooks.h"

namespace {

// Priority sits above the Arduino loopTask (1) but well below WiFi/BLE system
// tasks so we don't block radio work. Running at configMAX_PRIORITIES - 2 meant
// a long app-side operation (journal scan / AES decrypt) could starve IDLE1 on
// core 1, which the task watchdog catches as a panic → reboot. 8 is generous
// for UI responsiveness without the starvation hazard.
constexpr UBaseType_t kTaskPriority = 8;
constexpr BaseType_t  kTaskCore     = 1;
// Fallback re-check cadence while the display is off. Normally the task is
// woken instantly by hw_lvgl_task_notify_wake() (from ui_resume_timers() on
// wake or an editor-switch); this 1 s timeout is only a safety net so a
// missed notify still recovers, without waking 5×/s as the 200 ms value did.
constexpr uint32_t    kFakeSleepIdleMs = 1000;
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

void lvgl_task_fn(void *)
{
    for (;;) {
        if (ui_is_fake_sleep()) {
            // Nothing to render while the display is off. Block until a wake /
            // editor-switch notification arrives instead of spinning at 20 Hz;
            // the timeout is a safety net so a missed notify still gets
            // re-checked within kFakeSleepIdleMs.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kFakeSleepIdleMs));
            continue;
        }

        uint32_t next;
        {
            core::ScopedInstanceLock lock;
            next = lv_timer_handler();
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

void hw_lvgl_task_notify_wake()
{
    // Safe from any task context; a give while the task is running (not
    // blocked) simply leaves the notification pending for the next take.
    if (s_task) xTaskNotifyGive(s_task);
}

void hw_lvgl_task_start()
{
    if (s_task) return;

    BaseType_t ok = xTaskCreatePinnedToCore(
        lvgl_task_fn, "lvgl", kStackBytes, nullptr,
        kTaskPriority, &s_task, kTaskCore);
    if (ok != pdPASS) {
        log_e("lvgl_task: task create failed");
        s_task = nullptr;
    }
}

#else  // !ARDUINO

void hw_lvgl_task_start() {}
void hw_lvgl_task_notify_wake() {}

#endif  // ARDUINO
