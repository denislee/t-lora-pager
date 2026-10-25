/**
 * @file      gps_time_sync.cpp
 * @brief     GPS NMEA → wall-clock sync. See gps_time_sync.h.
 *
 * The GPS NMEA stream gives us UTC date+time. We set the system clock from
 * it, then write the local wall clock to the RTC exactly as the NTP path does
 * (factory.ino time_available() -> rtc.hwClockWrite(), i.e. localtime_r()
 * under the process TZ). That keeps the RTC contents consistent with what NTP
 * would have written for the same instant, DST included.
 */
#include "gps_time_sync.h"
#include "sensors.h"
#include "system.h"
#include "../core/kb_lock.h"

#include <cstdint>
#include <ctime>
#include <sys/time.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
#include <TinyGPS++.h>
#endif

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

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
// days_from_civil). ESP32 newlib ships no timegm(); this replaces the old
// TZ=UTC0 + mktime() round trip, which flipped the process-global TZ under
// every other task that calls localtime() (DC2.0.3).
int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                        // [0, 399]
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
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
        const time_t utc = (time_t)(days_from_civil(s_gps.date.year(),
                                                    s_gps.date.month(),
                                                    s_gps.date.day()) * 86400
                                    + s_gps.time.hour() * 3600
                                    + s_gps.time.minute() * 60
                                    + s_gps.time.second());

        struct timeval tv = { utc, 0 };
        settimeofday(&tv, nullptr);

        // Mirror the NTP path: write the user's local wall clock to the RTC.
        // TZ already holds the user's POSIX rule (factory.ino seeds it at boot,
        // timezone_set_user_tz() keeps it current), so localtime_r() of the
        // synced instant applies the right DST offset. It is evaluated after
        // settimeofday(), never against the pre-sync clock (DC2.0.3).
        if (hw_get_device_online() & HW_RTC_ONLINE) {
            struct tm local_tm;
            localtime_r(&utc, &local_tm);
            core::ScopedI2cLock i2c;  // P7.45
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
