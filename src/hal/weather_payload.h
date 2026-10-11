/**
 * @file      weather_payload.h
 * @brief     PagerLink weather.push payload <-> WeatherPayload codec.
 *
 * Pure (no I/O, no link, no LVGL, no Arduino) so the native Unity env can test
 * it (test/test_weather_payload). The hub sends one compact JSON object and the
 * Weather app hands the decoded ForecastCache + GeoData to the shared cache
 * (hal/weather_cache.cpp); the reverse direction re-serializes a stored payload.
 *
 * Wire shape (one JSON object, all integers):
 *   {"key":str,
 *    "ts":int,
 *    "hourly":[{"hr":int,"code":int,"temp":int,"pop":int}, ...],
 *    "daily":[{"date":"YYYY-MM-DD","code":int,"hi":int,"lo":int,"pop":int}, ...],
 *    "city"?:str, "lat"?:number, "lon"?:number}
 *
 * parse() rejects, never clamps: every field is range-checked and the first
 * violation is reported by name in `err` (e.g. "hourly[2].temp out of range").
 * hourly is capped at 8 and daily at 7 entries. city/lat/lon are optional; geo
 * is adopted only when all three are present and valid, otherwise has_geo
 * stays false (a partial geo is not an error).
 */
#pragma once

#include <cstddef>
#include <string>

#include "weather_cache.h"

// A decoded weather.push payload: cache key ("auto" for IP or the city name),
// the forecast, and — when the hub supplied all of city/lat/lon — the location.
struct WeatherPayload {
    std::string key;
    ForecastCache fc;
    GeoData geo;
    bool has_geo = false;
};

// Largest accepted cache key, in bytes (NVS blob stays small).
constexpr size_t WEATHER_KEY_MAX = 64;

// Downgrade UTF-8 to the basic-Latin subset Montserrat-10 can render: the
// degree sign is dropped, typographic dashes/quotes map to ASCII, Latin-1
// letters are de-accented, and anything unmapped is dropped. Pure copy of the
// sanitiser formerly private to apps/ui_weather.cpp.
std::string weather_sanitize_ascii(const std::string &in);

// Decode one weather.push object. Returns false and fills `err` (naming the
// offending field) on any violation; `out.fc` is zero-initialised first. On
// success fills out.key / out.fc / optionally out.geo, clears `err` and
// returns true.
bool weather_payload_parse(const std::string &json, WeatherPayload &out, std::string &err);

// Serialize `key`, `fc` and (when non-null) `geo` back to the wire shape, in
// the fixed member order key, ts, hourly, daily, city, lat, lon. Hourly/daily
// counts are capped at 8 / 7 and dates are written as "YYYY-MM-DD".
std::string weather_payload_encode(const std::string &key, const ForecastCache &fc,
                                   const GeoData *geo);
