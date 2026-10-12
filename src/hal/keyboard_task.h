/**
 * @file      keyboard_task.h
 * @brief     High-priority keyboard reader task.
 *
 * Decouples keyboard scanning from the main LVGL loop so key presses are
 * never dropped when the main task is busy (display flushes, NFC reader,
 * WiFi/BLE, audio). A dedicated FreeRTOS task polls the TCA8418 on a tight
 * schedule at higher-than-app priority and funnels events through a queue
 * that LVGL drains non-blocking.
 */
#pragma once

void hw_keyboard_task_start();

// Drop every key event still queued for LVGL and forget the held key (no
// stale auto-repeat). Called by enable_keyboard() when the keypad indev was
// disabled long enough that the backlog belongs to a previous screen
// (INPUT_STUTTER_PLAN.md IS.6). LVGL thread.
void hw_keyboard_flush_queue();
