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

// KITCHEN_BENCH=1: open the Kitchen, run its frame bench (KITCHEN_FPS_PLAN.md
// KF.1) on the fake clock with a trail check against full redraws, print the
// `[kitchen]` lines and return 0 (1: the bench did not finish, 2: trails).
int emu_kitchen_bench(lv_display_t *disp);
#endif
