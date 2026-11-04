/**
 * @file      input_focus.h
 * @brief     Tells background work whether the user is actively typing.
 *
 * Long-running LVGL timers (network polls, status-bar FFat walk, autosave)
 * run on the LVGL task under the instance mutex. The keyboard reader queues
 * keys without that mutex (IS.1), but they are only applied when the LVGL
 * task next runs, so slow work there still delays what the user sees.
 * Callbacks that do I/O should bail out early while a textarea is focused so
 * typing stays responsive.
 */
#pragma once

#include <lvgl.h>

namespace core {

// The first keypad input device's focused object that is an LVGL textarea, or
// nullptr. Safe to call from any LVGL timer callback.
lv_obj_t *focusedTextarea();

// True when focusedTextarea() finds one.
bool isTextInputFocused();

} // namespace core

