/**
 * @file      weather_link.h
 * @brief     Weather forecast over the PagerLink companion (weather.get).
 *
 * Runs the phone's fetch for one cache key (HUB_REMOVAL_PLAN.md H2.3 / W9):
 * asks the companion for the ForecastPayload of a key with weather.get
 * (PROTOCOL.md §6.3). This is the only header the weather UI includes for that
 * feature.
 *
 * Blocking: weather_link_get() drives hw_link_call(), which waits on the
 * phone's answer — see the threading note in link.h.
 *
 * The caller MUST check that the reply's key equals the requested key before
 * storing it; this function does not.
 */
#pragma once

#include <cstdint>
#include <string>

#include "result.h"
#include "weather_payload.h"

// weather.get waits for the phone to fetch/encode the forecast it has cached;
// named like chat.ask's 60 s but shorter — the phone is not running an LLM.
constexpr uint32_t WEATHER_GET_TIMEOUT_MS = 15000;

// Blocking: run on a worker task/thread, never the LVGL thread. On failure
// returns the HalError and fills err with a UI-ready message.
HalResult<WeatherPayload> weather_link_get(const std::string &key, std::string &err);
