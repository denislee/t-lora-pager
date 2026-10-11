/**
 * @file      weather_test.h
 * @brief     Emulator-only Weather test hook for `pagerlink smoke`.
 *
 * handlers_test.cpp (test.weather.fetch) drives this so the smoke can run the
 * Weather app's phone step (weather.get + key check + cache save) end to end
 * against the mock phone. Never compiled for hardware.
 */
#pragma once

#ifndef ARDUINO

#include <string>

namespace apps {

// Outcome of one weather_test_fetch().
struct WeatherTestResult {
    bool ok = false;
    std::string msg;         // the error the app would show after "Phone: "
    std::string city;        // location from the reply ("" when it had none)
    int hourly_count = 0;
    int daily_count = 0;
};

// Run the Weather app's phone step for `key` (blocking; a worker thread).
WeatherTestResult weather_test_fetch(const std::string &key);

}  // namespace apps

#endif  // !ARDUINO
