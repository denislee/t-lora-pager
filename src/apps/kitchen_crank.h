/**
 * @file      kitchen_crank.h
 * @brief     Where the wheel puts the Kitchen's tools.
 *
 * notches > 0 means the wheel rolled down (also key down / drag down), < 0
 * means up. The tool follows the wheel and stops at the end of its travel.
 */
#pragma once

#include <algorithm>
#include <cmath>

namespace kitchen_crank {

// The knife: 0 = knife up, travel = blade on the board.
inline float knife_target(float target, float notches, float travel)
{
    return std::max(0.0f, std::min(target + notches, travel));
}

// The peeler: the position only ever runs through a strip as stroke
// [start, bottom] then return [bottom, top]. Wheel down drives the peeler
// down the stroke, wheel up lifts it back through the return; it cannot go
// back past bottom or start.
inline float peeler_target(float target, float notches, float period, float peel_end)
{
    float k = floorf(target / period);
    float start = k * period;
    float bottom = start + period * peel_end;
    float top = start + period;
    bool returning = target >= bottom - 0.001f;
    if (notches >= 0) return returning ? std::max(target - notches, bottom) : std::min(target + notches, bottom);
    float n = -notches;
    return returning ? std::min(target + n, top) : std::max(target - n, start);
}

} // namespace kitchen_crank
