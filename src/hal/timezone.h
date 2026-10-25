/**
 * @file      timezone.h
 * @brief     The user's timezone: IANA name + POSIX TZ rule (DC2.4.17).
 *
 * Implemented in src/ui_time_sync.cpp next to the bundled zone table. LVGL-
 * free, so HAL, link and app code share this one set of declarations instead
 * of each forward-declaring the functions it needs.
 */
#pragma once

#include <string>
#include <vector>

// The user's IANA zone name; "America/Sao_Paulo" when nothing is stored.
std::string timezone_get_user_tz();

// Persist `tz` ("" or nullptr clears it back to the default) and apply its
// POSIX rule to the process TZ env (setenv + tzset) at once.
void timezone_set_user_tz(const char *tz);

// The POSIX TZ rule for the user's zone (or the default), e.g. for
// configTzTime() or the boot-time setenv("TZ", ...).
const char *timezone_get_user_posix();

// Fills `out` with the bundled IANA zone names (replaces its contents).
void timezone_fetch_list(std::vector<std::string> &out);
