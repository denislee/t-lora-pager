/**
 * @file      gps_time_sync.cpp
 * @brief     GPS NMEA → wall-clock sync. See gps_time_sync.h.
 *
 * The GPS NMEA stream gives us UTC date+time; we apply the user's POSIX
 * timezone rule (resolved by ui_time_sync.cpp::timezone_fetch_offset) to
 * compute local wall-clock, then drive the same settimeofday + RTC write
 * path that the NTP sync uses. That keeps the RTC contents consistent
 * with what NTP would have written for the same wall time.
 */
#include "gps_time_sync.h"
#include "sensors.h"
#include "system.h"

#include <string>
#include <ctime>
#include <sys/time.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
#include <TinyGPS++.h>
#endif

// Defined in ui_time_sync.cpp. Forward-declared here so the HAL layer does
// not have to pull in apps/settings_internal.h.
std::string timezone_get_user_tz();
bool        timezone_fetch_offset(const char *tz, int &offset_sec,
                                  std::string &err);

namespace {

#ifdef ARDUINO
TinyGPSPlus s_gps;
#endif

// -2 = not supported, -1 = timed out, 0 = idle/in-progress, 1 = synced.
int  s_status = 0;

#ifdef ARDUINO
// True while a sync attempt is running. The GPS rail is off at boot and has
// no other owner, so it is powered exactly while s_active is true: on in
// hw_start_time_sync_gps(), off on fix, timeout or hw_stop_time_sync_gps().
bool s_active = false;

uint32_t s_deadline_ms = 0;

// Convert a UTC struct tm to time_t portably. ESP32 newlib doesn't ship
// timegm(), so we temporarily switch TZ to UTC and let mktime() interpret
// the broken-down time as UTC. The previous TZ env var is restored before
// returning so callers that rely on localtime() don't observe a flicker.
time_t utc_tm_to_epoch(struct tm &utc_tm)
{
    const char *prev = getenv("TZ");
    std::string saved = prev ? prev : "";
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&utc_tm);
    if (prev) setenv("TZ", saved.c_str(), 1);
    else      unsetenv("TZ");
    tzset();
    return epoch;
}
#endif

} // namespace

bool hw_start_time_sync_gps()
{
#ifdef ARDUINO
    // A restart while a sync is already running keeps the rail up rather
    // than cycling it (and the UART) off and on again.
    if (!s_active) hw_set_gps_powered(true);

    // TinyGPSPlus has no public reset; reconstruct in place. Default ctor
    // zeroes all the latched fields so a stale fix from a prior session
    // can't satisfy our validity check before fresh NMEA arrives.
    s_gps = TinyGPSPlus();

    while (Serial1.available()) (void)Serial1.read();

    s_active = true;
    s_status = 0;
    // Cold-start TTFF on a generic L76K-class module is typically <60s with
    // a clear sky; allow some headroom for first-power-on cases.
    s_deadline_ms = millis() + 90000;
    return true;
#else
    s_status = -2;
    return false;
#endif
}

void hw_stop_time_sync_gps()
{
#ifdef ARDUINO
    if (!s_active) return;
    s_active = false;
    hw_set_gps_powered(false);
#endif
}

void hw_pump_time_sync_gps()
{
#ifdef ARDUINO
    if (!s_active) return;

    while (Serial1.available()) {
        s_gps.encode((char)Serial1.read());
    }

    // Wait for a freshly-committed RMC/ZDA that gives us both date and time
    // together. The age() bound rejects sentences whose date was committed
    // long before the matching time (or vice versa), and the year sanity
    // check guards against modules that briefly emit 1980-01-06 from their
    // internal RTC before acquiring a real fix.
    bool fresh = s_gps.date.isValid() && s_gps.time.isValid()
                 && s_gps.date.age() < 2000 && s_gps.time.age() < 2000
                 && s_gps.date.year() >= 2024;
    if (fresh) {
        struct tm utc_tm = {};
        utc_tm.tm_year  = s_gps.date.year() - 1900;
        utc_tm.tm_mon   = s_gps.date.month() - 1;
        utc_tm.tm_mday  = s_gps.date.day();
        utc_tm.tm_hour  = s_gps.time.hour();
        utc_tm.tm_min   = s_gps.time.minute();
        utc_tm.tm_sec   = s_gps.time.second();
        utc_tm.tm_isdst = 0;

        time_t utc = utc_tm_to_epoch(utc_tm);

        std::string tz = timezone_get_user_tz();
        int offset_sec = 0;
        std::string err;
        bool have_offset = timezone_fetch_offset(tz.c_str(), offset_sec, err);

        struct timeval tv = { utc, 0 };
        settimeofday(&tv, nullptr);

        // Mirror the NTP path: write the user's local wall-clock to the
        // RTC chip. hwClockWrite() itself calls localtime_r() under the
        // active TZ env, so we'd ordinarily want to setenv("TZ", ...) and
        // let it do the conversion. We do the offset arithmetic explicitly
        // instead so this path doesn't perturb the global TZ env on builds
        // that don't otherwise touch it.
        if (hw_get_device_online() & HW_RTC_ONLINE) {
            struct tm local_tm;
            if (have_offset) {
                time_t local = utc + offset_sec;
                gmtime_r(&local, &local_tm);
            } else {
                local_tm = utc_tm;
            }
            instance.rtc.setDateTime(local_tm);
        }

        s_status = 1;
        s_active = false;
        hw_set_gps_powered(false);
        return;
    }

    if ((int32_t)(millis() - s_deadline_ms) > 0) {
        s_status = -1;
        s_active = false;
        hw_set_gps_powered(false);
    }
#endif
}

int hw_get_time_sync_gps_status()
{
    return s_status;
}
