/**
 * @file      weather_cache.h
 * @brief     Weather location + forecast cache (NVS on hardware, in-memory elsewhere).
 *
 * Extracted from apps/ui_weather.cpp (IOS_COMPANION_PLAN.md Track C 9) so the
 * PagerLink weather.push handler and the Weather app share one store.
 *
 * The forecast is cached as a compact binary blob (NVS key "fcd"): the subset
 * the Weather app renders (8 hourly cells, 7 daily cells) packs into a fixed
 * struct. Both caches are keyed by weather_cache_key(): "auto" for IP-based
 * lookups or the user-supplied city name, so switching location invalidates
 * them automatically. WEATHER_CACHE_VER is bumped if the stored schema changes.
 *
 * ARDUINO: Preferences namespace WEATHER_PREFS_NS (shared with the user-city
 * keys ui_weather.cpp owns). Emulator / native: a process-local, mutex-guarded
 * in-memory store (no file I/O), so the emulator can exercise the cache.
 *
 * LVGL-free and Arduino-free header.
 */
#pragma once

#include <cstdint>
#include <string>

#define WEATHER_PREFS_NS       "weather"
#define WEATHER_CACHE_VER      1

struct GeoData {
    std::string location;
    double lat = 0, lon = 0;
};

struct CacheHour {
    uint8_t hr;      // 0-23
    uint8_t code;    // WMO weather code
    int8_t  temp;    // Celsius
    uint8_t pop;     // precipitation probability, 0-100
};

struct CacheDay {
    char    date[11];   // "YYYY-MM-DD" + NUL — consumed by weekday_from_date()
    uint8_t code;
    int8_t  hi;
    int8_t  lo;
    uint8_t pop;
};

struct ForecastCache {
    uint32_t  ts;           // unix time of fetch (0 = unknown clock)
    uint8_t   hourly_count;
    uint8_t   daily_count;
    CacheHour hourly[8];
    CacheDay  daily[7];
};

// Stored verbatim as the NVS blob "fcd": the layout must never drift.
static_assert(sizeof(ForecastCache) == 144, "ForecastCache NVS blob layout changed");

// Cache key for a user-city setting: "" (auto / IP geolocation) -> "auto".
std::string weather_cache_key(const std::string &user_city);

// Location cache. load returns false when absent, stale-versioned or keyed
// for a different source than `want_key`.
bool weather_cache_load_geo(const std::string &want_key, GeoData &out);
void weather_cache_save_geo(const std::string &key, const GeoData &in);

// Forecast cache, same keying rules as the location cache.
bool weather_cache_load_forecast(const std::string &want_key, ForecastCache &out);
void weather_cache_save_forecast(const std::string &key, const ForecastCache &in);

// Change signal: bumped on every weather_cache_save_forecast() (any key, any
// task). The Weather app compares it to skip its own saves.
uint32_t weather_cache_generation();

// P7.47: called (from the saving task) after every generation bump; one
// hook, nullptr to clear. The Weather app uses it to re-render a
// weather.push without polling.
void weather_cache_set_change_hook(void (*hook)());
