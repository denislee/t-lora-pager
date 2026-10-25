/**
 * @file      lvgl_task.h
 * @brief     Dedicated LVGL handler task pinned to core 1.
 *
 * Moves `lv_timer_handler()` and the `core::System` tick out of
 * `loop()` onto a pinned high-priority FreeRTOS task, so the Arduino
 * loopTask can't be blocked by whatever else ends up sharing its
 * cadence. Must be started AFTER `beginLvglHelper()` and after
 * `core::System::getInstance().init()`.
 */
#pragma once
#include <stdint.h>

void hw_lvgl_task_start();

// BT.11: hw_init() hands the display brightness here instead of turning the
// backlight on before any UI exists. The LVGL task applies it once, after the
// first lv_timer_handler() pass that flushed (a full frame is on the panel by
// then), so boot goes dark -> UI with no lit garbage/black panel in between.
// Call before hw_lvgl_task_start().
void hw_lvgl_task_set_boot_backlight(uint8_t level);
