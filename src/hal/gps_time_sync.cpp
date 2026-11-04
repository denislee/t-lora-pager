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
#include "nmea_rmc.h"
#include "sensors.h"
#include "system.h"
#include "../core/kb_lock.h"

#include <cstdint>
#include <ctime>
#include <sys/time.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <LilyGoLib.h>
#endif

namespace {

#ifdef ARDUINO
hal::NmeaRmcClock s_rmc;
#endif

// -2 = not supported, -1 = timed out, 0 = idle/in-progress, 1 = synced.
int  s_status = 0;

#ifdef ARDUINO
// True while a sync attempt is running. The GPS rail is off at boot and has
// no other owner, so it is powered exactly while s_active is true: on in
// hw_start_time_sync_gps(), off on fix, timeout or hw_stop_time_sync_gps().
bool s_active = false;

uint32_t s_deadline_ms = 0;
#endif

} // namespace

bool hw_start_time_sync_gps()
{
#ifdef ARDUINO
    // A restart while a sync is already running keeps the rail up rather
    // than cycling it (and the UART) off and on again.
    if (!s_active) hw_set_gps_powered(true);

    s_rmc.reset();

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

    // Drain the UART fully and keep the last accepted value: a pump that
    // fell behind must set the clock from the newest sentence, not an older
    // one still buffered ahead of it.
    bool got = false;
    int64_t utc = 0;
    while (Serial1.available()) {
        if (s_rmc.feed((char)Serial1.read(), &utc)) got = true;
    }

    if (got) {
        const time_t utc_secs = (time_t)utc;

        struct timeval tv = { utc_secs, 0 };
        settimeofday(&tv, nullptr);

        // Mirror the NTP path: write the user's local wall clock to the RTC.
        // TZ already holds the user's POSIX rule (factory.ino seeds it at boot,
        // timezone_set_user_tz() keeps it current), so localtime_r() of the
        // synced instant applies the right DST offset. It is evaluated after
        // settimeofday(), never against the pre-sync clock (DC2.0.3).
        if (hw_get_device_online() & HW_RTC_ONLINE) {
            struct tm local_tm;
            localtime_r(&utc_secs, &local_tm);
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

bool hw_time_sync_gps_active()
{
#ifdef ARDUINO
    return s_active;
#else
    return false;
#endif
}
