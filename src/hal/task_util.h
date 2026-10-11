/**
 * @file      task_util.h
 * @brief     Shared boilerplate for the HAL's long-lived FreeRTOS tasks
 *            (keyboard, rotary, nfc, lvgl, charge).
 *
 * Hardware-only: every user is itself compiled only on ARDUINO builds.
 * Each task still owns its own name, stack size, priority and core; these
 * helpers only remove the copy-pasted create / notify / park / enqueue
 * bodies around them.
 */
#pragma once

#ifdef ARDUINO

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "../core/system_hooks.h"
#include "system.h"

namespace hal {

// Safety-net timeout for a task parked on its notification while it has
// nothing to do (display off / feature idle). The real wake path is a task
// notify (ui_resume_timers(), hw_start_nfc_discovery(), ...); this only
// bounds recovery if one is ever missed.
constexpr uint32_t kTaskParkMs = 1000;

// Create `fn` pinned to `core` with no argument. On failure logs
// "<tag>: task create failed", leaves *handle null and returns false.
inline bool start_pinned_task(TaskFunction_t fn, const char *name,
                              uint32_t stack_bytes, UBaseType_t priority,
                              BaseType_t core, TaskHandle_t *handle,
                              const char *tag)
{
    if (xTaskCreatePinnedToCore(fn, name, stack_bytes, nullptr, priority,
                                handle, core) == pdPASS) {
        return true;
    }
    log_e("%s: task create failed", tag);
    *handle = nullptr;
    return false;
}

// Wake a task blocked in ulTaskNotifyTake. Safe from any task context; a
// give while the task is running (not blocked) simply leaves the
// notification pending for the next take. No-op before the task exists.
inline void notify_task(TaskHandle_t task)
{
    if (task) xTaskNotifyGive(task);
}

// Block the calling task on its notification for up to kTaskParkMs.
inline void park_task()
{
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTaskParkMs));
}

// While the display is off (fake-sleep) park the calling task and return
// true, so the caller's loop can `continue` and re-check; false otherwise.
inline bool park_if_fake_sleep()
{
    if (!ui_is_fake_sleep()) return false;
    park_task();
    return true;
}

// Non-blocking enqueue that drops the oldest pending item when the queue is
// full — better to lose a stale input event than to stall the reader.
template <typename T>
inline void queue_send_drop_oldest(QueueHandle_t q, const T &item)
{
    if (xQueueSend(q, &item, 0) != pdTRUE) {
        T discard;
        xQueueReceive(q, &discard, 0);
        xQueueSend(q, &item, 0);
    }
}

} // namespace hal

// An input event was just queued: boost the CPU for responsiveness and wake
// the LVGL task immediately so it renders without waiting for its kMaxTickMs
// (200 ms) fallback sleep. xTaskNotifyGive is cheap and idempotent — several
// gives before the task's ulTaskNotifyTake collapse to one — and is safe
// while the caller holds the instance mutex (the LVGL task simply blocks on
// it until released).
inline void hw_input_event_wake()
{
    hw_cpu_boost_for_input();
    hw_lvgl_task_notify_wake();
}

#endif // ARDUINO
