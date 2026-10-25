/**
 * @file      ui_define.h
 * @brief     Back-compat aggregator. Prefer the focused ui/* headers in
 *            new code:
 *              ui/widgets.h     — themed lv_*_create wrappers
 *              ui/theme.h       — color tokens, radius, theme_init
 *              ui/fonts.h       — per-context font getters
 *              ui/modals.h      — popups, message boxes, loading, prompts
 *              ui/back_button.h — shared top-bar back button
 *
 * This header re-exports all of them plus the LVGL/Arduino/LilyGoLib
 * transitive bag that the older code relied on. It also still owns a few
 * cross-module hooks that don't have a better home yet.
 */
#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
/* WiFi.h arrives transitively via hal/types.h on both platforms.
 * esp_mac.h was unused here — moved to hal/system.cpp where it belongs. */
#endif
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "hal_interface.h"
#include "core/system_hooks.h"

#include "ui/theme.h"
#include "ui/fonts.h"
#include "ui/widgets.h"
#include "ui/modals.h"
#include "ui/back_button.h"

/* Cross-module hooks that don't belong in a feature header. */
lv_indev_t *lv_get_encoder_indev();
lv_indev_t *lv_get_keyboard_indev();
void set_default_group(lv_group_t *group);

void disable_keyboard();
void enable_keyboard();

/* Open `path` in the Editor after the current event unwinds (switches to the
 * Editor, IS.5). */
void ui_text_editor_open_file_deferred(const char *path);

/* Editor / fake-sleep / instance-lock hooks are declared in
 * core/system_hooks.h (included at top of this file). */
