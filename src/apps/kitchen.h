/**
 * @file      kitchen.h
 * @brief     Emulator hooks into the Kitchen game (ui_kitchen.cpp) for the
 *            screenshot sweep and the frame bench (KITCHEN_FPS_PLAN.md KF.1,
 *            KF.2). Each call needs the Kitchen app to be open.
 */
#pragma once

#ifndef ARDUINO
namespace kitchen {
int shot_count();
// Puts the game in seeded state i with the frame timer paused (so it holds
// still for the shot); returns the state's name, or null.
const char *shot_setup(int i);
void shot_release();
// Invalidate the whole stage (trail check).
void debug_invalidate_all();
// The KF.1 bench: a fixed seed, a steady crank through both stages, per-phase
// totals printed as `[kitchen] bench ...` lines at the end.
void bench_start();
// KF.4: time and accuracy of each fast-cast shortcut on the potato shown now.
void cast_check();
// Mute the frame probe (the trail check's forced redraws are not frames).
void probe_mute(bool on);
bool bench_done();
} // namespace kitchen
#endif

#if defined(INPUT_PROFILE) || defined(KITCHEN_BENCH) || !defined(ARDUINO)
#include <stdint.h>
namespace kitchen {
// One display flush for the frame probe (KF.24). blocked_us = how long the LVGL task was held up by this flush.
void note_flush(uint32_t px, uint32_t swap_us, uint32_t push_us, uint32_t blocked_us);
}
#endif
