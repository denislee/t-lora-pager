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
// Turn the wheel by n notches / invalidate the whole stage (trail check).
void debug_crank(float notches);
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
