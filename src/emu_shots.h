/**
 * @file      emu_shots.h
 * @brief     Emulator-only screenshot sweep: every app (and every Settings
 *            subpage) in every UI style, with dark mode off and on.
 */
#pragma once

#ifndef ARDUINO
#include <lvgl.h>

// Writes <dir>/<style>-<light|dark>/<NN>-<page>.ppm and returns the number of
// pages that failed to render (0 = all captured).
int emu_shots_run(lv_display_t *disp, const char *dir);
#endif
