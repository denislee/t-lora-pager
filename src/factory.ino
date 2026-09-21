/**
 * @file      factory.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-04
 *
 */
#ifdef ARDUINO
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include "hal_interface.h"
#include "hal/notes_crypto.h"
#include "event_define.h"
#include "core/system_hooks.h"
#include "core/input_focus.h"

// Vendor entry point: sets a volatile flag consumed by the rotary task to run
// perform_fake_sleep_toggle() — the same path as a long-press. Declared here
// rather than in a header because it is only used from this translation unit.
extern "C" void lilygo_request_fake_sleep_toggle();

#include <string>

std::string timezone_get_user_tz();
bool timezone_fetch_offset(const char *tz, int &raw_offset_sec, int &dst_offset_sec, std::string &err);
const char *timezone_get_user_posix();



/* Defined in ui_lock.cpp — shows the unlock modal if crypto is enabled and
 * the session is locked. No-op otherwise. */
void ui_device_lock_enforce();

static const char *ntpServer1 = "pool.ntp.org";
static const char *ntpServer2 = "time.nist.gov";
static const uint64_t  gmtOffset_sec = GMT_OFFSET_SECOND;
static const int   daylightOffset_sec = 0;

// Callback function (gets called when time adjusts via NTP)
static void time_available(struct timeval *t)
{
    Serial.println("Got time adjustment from NTP!");
    // printLocalTime();
    if (instance.getDeviceProbe() & HW_RTC_ONLINE) {
        instance.rtc.hwClockWrite();
    }
    hw_notify_time_sync_completed();
}

// WARNING: This function is called from a separate FreeRTOS task (thread)!
// P4.19: set by WiFiGotIP to trigger NTP backoff / attempt-count reset on
// the next loop() tick. Volatile because it crosses the WiFi task boundary.
static volatile bool s_ntp_new_assoc = false;

void WiFiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));
    /* NTP sync is driven from the main loop's poll (see loop()) rather than
     * from this event callback, so boot-time flows that connect WiFi before
     * the event handler is fully wired are still covered. */
    // P4.19: signal loop() to reset NTP backoff and attempt count for the
    // new association. Don't manipulate static loop() state from this task.
    s_ntp_new_assoc = true;
}

#include "core/system.h"
#include "apps/app_registry.h"
#include "hal/lvgl_task.h"
#include "hal/nfc_task.h"
#include "hal/rotary_task.h"
#include "hal/charge_task.h"

void setup()
{
    hw_set_cpu_freq(240);

    Serial.begin(115200);

    core::instance_lock_init();

    hw_load_setting();
    user_setting_params_t settings;
    hw_get_user_setting(settings);

    // hw_set_cpu_freq() is a no-op when the freq is already set, so the
    // common case (settings.cpu_freq_mhz == 240) incurs zero PLL re-init cost.
    hw_set_cpu_freq(settings.cpu_freq_mhz);

    uint32_t disable_flags = 0;
    // The vendor begin() I2C scan is a debug aid that costs ~100+ ms of I2C
    // probing + serial dump per scan — skip it unconditionally.
    disable_flags |= NO_SCAN_I2C_DEV;
    // Vendor GPS init (UART handshake against its own gps object) is always
    // skipped: nothing in this codebase reads HW_GPS_ONLINE or touches
    // instance.gps — hw_start_time_sync_gps() talks to Serial1 directly with
    // its own TinyGPSPlus instance, transiently, on demand (OPTIMIZATION_PHASE3.md P2.5).
    disable_flags |= NO_HW_GPS;
    if (!settings.nfc_enable) disable_flags |= NO_HW_NFC;
    if (!settings.haptic_enable) disable_flags |= NO_HW_DRV;

    instance.begin(disable_flags);

    beginLvglHelper(instance);

    // Arm the SNTP and WiFi event callbacks BEFORE connectivity bring-up —
    // hw_connectivity_init() (called near the end of setup(), after the LVGL
    // task starts) calls hw_set_wifi_enable() which in turn fires WiFi.begin()
    // from saved credentials. Registering the notification callback after
    // WiFi.begin() races the first DHCP/GOT_IP event and can miss the sync
    // that kicks off on cold boot, leaving the clock stuck at 1970.
    sntp_set_time_sync_notification_cb(time_available);
    WiFi.onEvent(WiFiGotIP, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.setAutoReconnect(false);

    // Apply the saved POSIX TZ rule to the C library env BEFORE hw_init()
    // reads the RTC. hw_init() pushes the RTC's broken-down time through
    // mktime() + settimeofday(); mktime() interprets its argument as local
    // time under the active TZ, so an unset TZ here would silently shift
    // the system clock by the user's UTC offset (the symptom: localtime()
    // returns UTC+8 from the GMT_OFFSET_SECOND fallback until NTP runs).
    setenv("TZ", timezone_get_user_posix(), 1);
    tzset();

    hw_init();

    apps::register_all();
    core::System::getInstance().init();

    // Passphrase-protected notes: if the session is locked, put an unlock
    // modal on top of the UI before rendering starts. The modal has no cancel
    // button so the device stays gated until the user enters the passphrase.
    ui_device_lock_enforce();

    // LVGL rendering and NFC polling now run on their own FreeRTOS tasks so
    // the main loop's mutex hold window stays short. Must come after
    // core::System::getInstance().init() — the LVGL task calls System::loop().
    hw_lvgl_task_start();
    hw_nfc_task_start();
    hw_rotary_task_start();
    hw_charge_task_start();

    // Bring up WiFi/BLE/LoRa/NFC only after the LVGL task is rendering so
    // stack bring-up (~0.5–1 s per radio when enabled) doesn't delay the
    // first frame. The SNTP/WiFi callbacks were armed earlier in setup(),
    // so the boot-time NTP sync path is unaffected; the NFC task tolerates
    // discovery starting late (same as a runtime toggle from Settings).
    hw_connectivity_init();

    Serial.println("Start done. run main loop");
}

// P4.9: VBUS/charging probe for the auto-sleep guard. Kept out of the hot path
// — only called on the tick where every other sleep precondition already holds.
static bool is_on_external_power()
{
    monitor_params_t p;
    hw_get_monitor_params(p);
    return p.is_charging;
}

void loop()
{
    // Poll-based NTP driver: keep re-triggering configTime() while WiFi is
    // up and the clock has not actually synced yet. A single trigger is not
    // enough — lwIP's SNTP will retry on its own, but on a cold boot the
    // first DNS lookup or UDP round-trip often drops (AP still handshaking,
    // DHCP lease just arrived, etc.) and we want to force a fresh cycle
    // until the time_available() callback fires.
    //
    // P4.19: exponential backoff (30s → 1min → 5min → 15min cap) and a hard
    // per-association attempt ceiling so a persistent NTP failure doesn't
    // burn the radio and CPU indefinitely. Both reset on each GOT_IP event.
    // Timezone offset is cached — it changes twice a year, not twice a minute.
    static uint32_t last_ntp_attempt_ms   = 0;
    static uint32_t ntp_backoff_ms        = 30000;   // starts at 30 s
    static int      ntp_attempts          = 0;        // per WiFi association
    static bool     tz_offset_valid       = false;
    static int      cached_tz_offset_sec  = 0;
    static constexpr int kNtpMaxAttempts  = 10;       // ~30s+1m+5m+15m*7 ≈ 2h cap
    static constexpr uint32_t kNtpBackoffCap = 15UL * 60 * 1000;  // 15 min

    if (s_ntp_new_assoc) {
        // New DHCP lease — reset backoff and attempt count for this association.
        s_ntp_new_assoc    = false;
        last_ntp_attempt_ms = 0;
        ntp_backoff_ms      = 30000;
        ntp_attempts        = 0;
        // Keep tz_offset_valid: the timezone hasn't changed between associations.
    }

    if (hw_get_time_sync_status() == 0 && hw_get_wifi_connected()
            && ntp_attempts < kNtpMaxAttempts) {
        uint32_t now = millis();
        if (last_ntp_attempt_ms == 0 || now - last_ntp_attempt_ms >= ntp_backoff_ms) {
            log_i("Triggering NTP sync (attempt %d, backoff %lu ms)",
                  ntp_attempts + 1, (unsigned long)ntp_backoff_ms);
            if (!tz_offset_valid) {
                std::string tz = timezone_get_user_tz();
                if (!tz.empty()) {
                    int raw = 0, dst = 0;
                    std::string err;
                    if (timezone_fetch_offset(tz.c_str(), raw, dst, err)) {
                        cached_tz_offset_sec = raw + dst;
                        tz_offset_valid      = true;
                    }
                }
            }
            if (tz_offset_valid && cached_tz_offset_sec != 0) {
                hw_start_time_sync_ntp(cached_tz_offset_sec);
            } else {
                hw_start_time_sync_ntp();
            }

            last_ntp_attempt_ms = now;
            ntp_attempts++;
            // Advance backoff: 30s → 60s → 300s → 900s (cap at kNtpBackoffCap).
            ntp_backoff_ms = min(ntp_backoff_ms * 2, (uint32_t)kNtpBackoffCap);
            if (ntp_backoff_ms < 60000 && ntp_attempts >= 2) ntp_backoff_ms = 60000;
        }
    }

    uint32_t inactive_time = 0;
    {
        core::ScopedInstanceLock lock;
        instance.loop();
        if (!ui_is_fake_sleep()) {
            inactive_time = lv_display_get_inactive_time(NULL);
        }
    }

    // Latch: true after lilygo_request_fake_sleep_toggle() is called but before
    // the rotary task has consumed the flag and ui_is_fake_sleep() becomes true.
    // Prevents loop() re-requesting the toggle on every 50 ms tick and bouncing
    // the device back out of sleep.
    static bool auto_sleep_pending = false;

    if (ui_is_fake_sleep()) {
        // Clear the latch: the rotary task processed the request and the
        // device is now confirmed in fake sleep.
        if (auto_sleep_pending) auto_sleep_pending = false;
        // BLE and WiFi both need ≥80MHz; hold there while either link is
        // up so the fake-sleep power saving doesn't drop them.
        // hw_set_cpu_freq() is a no-op when hw_power_down_all() already set
        // this frequency; loop() just keeps it correct if the link state
        // changes mid-sleep. P5.4: the rule itself lives in
        // hw_fake_sleep_target_freq() so this and hw_power_down_all() agree.
        hw_set_cpu_freq(hw_fake_sleep_target_freq());
    } else {
        // Settings are an in-memory struct, but the loop runs at 20 Hz —
        // refreshing the user-configured CPU freq once per second is plenty
        // and keeps the cached active_freq path branch-only on most ticks.
        // disp_timeout_second, brightness_level and keyboard_bl_level are
        // cached in the same pass so the dim ladder and auto-sleep see the
        // current setting without a second struct copy per tick.
        static uint32_t last_settings_refresh_ms = 0;
        static uint32_t cached_active_freq = 240;
        static uint8_t  cached_disp_timeout_sec = 0;
        static uint8_t  cached_brightness = 8;       // P4.3: user display level
        static uint8_t  cached_kb_brightness = 8;    // P4.3: user keyboard level

        uint32_t now_ms = millis();
        if (cached_active_freq == 0 || now_ms - last_settings_refresh_ms > 1000) {
            user_setting_params_t settings;
            hw_get_user_setting(settings);
            cached_active_freq       = settings.cpu_freq_mhz;
            cached_disp_timeout_sec  = settings.disp_timeout_second;
            cached_brightness        = settings.brightness_level;
            cached_kb_brightness     = settings.keyboard_bl_level;
            last_settings_refresh_ms = now_ms;
        }

        if (inactive_time > 2000 && cached_active_freq > 80) {
            hw_set_cpu_freq(80);
        } else {
            hw_set_cpu_freq(cached_active_freq);
        }

        // P4.3: dim ladder — at ~60% of the display timeout, lower the
        // display and keyboard backlights to save power while giving the user
        // a visual warning that sleep is imminent. Restore immediately on any
        // LVGL activity (inactive_time dropped back below the dim threshold).
        // The AW9364 driver early-returns on unchanged values before clamping,
        // so we cache what we actually wrote and compare before writing again.
        static bool     s_dimmed = false;
        static uint8_t  s_dim_disp_written = 0;   // last value sent to display
        static uint8_t  s_dim_kb_written   = 0;   // last value sent to KB LED
        static uint32_t s_prev_inactive_time = 0;

        if (cached_disp_timeout_sec > 0) {
            uint32_t dim_threshold_ms = (uint32_t)cached_disp_timeout_sec * 600; // 60%
            bool activity = (inactive_time < s_prev_inactive_time); // LVGL reset counter
            if (s_dimmed && activity) {
                // Restore on activity — only write if we need to change the level.
                uint8_t want_disp = cached_brightness;
                uint8_t want_kb   = cached_kb_brightness;
                if (s_dim_disp_written != want_disp) {
                    hw_set_disp_backlight(want_disp);
                    s_dim_disp_written = want_disp;
                }
                if (s_dim_kb_written != want_kb) {
                    hw_set_kb_backlight(want_kb);
                    s_dim_kb_written = want_kb;
                }
                s_dimmed = false;
            } else if (!s_dimmed && inactive_time >= dim_threshold_ms) {
                // Enter dim — write dim levels.
                uint8_t dim_disp = (cached_brightness > 2) ? cached_brightness / 3 : 1;
                uint8_t dim_kb   = (cached_kb_brightness > 2) ? cached_kb_brightness / 3 : 1;
                if (s_dim_disp_written != dim_disp) {
                    hw_set_disp_backlight(dim_disp);
                    s_dim_disp_written = dim_disp;
                }
                if (s_dim_kb_written != dim_kb) {
                    hw_set_kb_backlight(dim_kb);
                    s_dim_kb_written = dim_kb;
                }
                s_dimmed = true;
            } else if (!s_dimmed) {
                // Not dimmed and not at threshold: make sure cache stays in sync
                // with the current user setting (handles settings changes while awake).
                s_dim_disp_written = cached_brightness;
                s_dim_kb_written   = cached_kb_brightness;
            }
        }
        s_prev_inactive_time = inactive_time;

        // Auto fake-sleep: fire the vendor toggle when the LVGL inactivity
        // clock exceeds the user's display-timeout setting and no guarded
        // activity is in progress. The async latch prevents loop() from
        // re-requesting the toggle on every subsequent 50 ms tick before the
        // rotary task has time to process it and set ui_is_fake_sleep().
        if (!auto_sleep_pending
                && cached_disp_timeout_sec > 0
                && inactive_time >= (uint32_t)cached_disp_timeout_sec * 1000
                && !hw_player_running()
                && !hw_rec_running()
                && !core::isTextInputFocused()
                && !ssh_session_is_active()
                // P4.9: do not blank the screen mid-transfer — the "Unsafe to
                // disconnect" overlay lives on lv_layer_top() and would go
                // invisible. Evaluated last so the (cheap) checks above
                // short-circuit it on all but the one tick that would sleep.
                && !hw_is_usb_msc_mounted()
                // P4.9: do not auto-sleep on external power. Deliberate product
                // call: a docked device stays awake. Note this means P4.5's
                // in-sleep 80 % cap enforcement only covers the sleep-then-plug-in
                // order — the plug-in-then-idle order never reaches fake sleep at
                // all. hw_get_monitor_params() is TTL-cached (1 s charging / 5 s
                // discharging), and the short-circuit above means it is only read
                // on a tick that would otherwise sleep.
                && !is_on_external_power()) {
            lilygo_request_fake_sleep_toggle();
            auto_sleep_pending = true;
        }

        // P4.3: restore full brightness when waking (s_dimmed may have been
        // set just before the sleep threshold fired). The restore-on-wake path
        // runs through ui_resume_timers() -> hw_power_up_all() ->
        // hw_set_disp_backlight(user_setting.brightness_level), so the
        // hardware is already correct; we just need to clear our dim state so
        // the next awake cycle doesn't mis-detect "activity" from the counter
        // wrapping at zero.
        if (auto_sleep_pending && s_dimmed) {
            s_dimmed = false;
            // Hardware restore is handled by hw_power_up_all() on wake.
        }
    }

    // P4.17: drive the WiFi reconnect supervisor. It self-throttles internally
    // (30 s -> 1 min -> 5 min backoff, then WIFI_OFF + a 15 min retry after five
    // consecutive failures), so calling it every loop() iteration is correct and
    // cheap. Without it an AP that goes away is never recovered, and the modem
    // stays powered for zero connectivity because both power-save wrappers guard
    // on hw_get_wifi_connected().
    hw_wifi_supervise(millis());

    // Idle cadence. This loop is pure housekeeping (NTP re-trigger, vendor
    // instance.loop(), CPU-freq management) — LVGL rendering and rotary/NFC
    // input each run on their own FreeRTOS tasks. During fake-sleep the display
    // is off and nothing here needs 20 Hz, so back off hard to cut the
    // always-on I2C/PMU polling and dynamic-power draw of the busiest task.
    // Wake is driven entirely by the separate rotary task, so display/UI
    // responsiveness on wake is unaffected.
    //
    // P5.2: 2 Hz -> 0.5 Hz while asleep. Everything this loop still drives in
    // that state is either self-throttled on a far longer clock (NTP backoff
    // is 30 s at its fastest, hw_wifi_supervise() 30 s) or idempotent
    // (hw_set_cpu_freq() no-ops when the frequency is unchanged). The vendor
    // instance.loop() only dispatches HW_IRQ_RTC and HW_IRQ_SENSOR: the IMU is
    // unregistered on sleep entry so no sensor interrupt can arrive, and
    // RTC_EVENT_INTERRUPT has no registered consumer anywhere in src/. So the
    // extra 1.5 s of latency is latency on work that does not exist.
    delay(ui_is_fake_sleep() ? 2000 : 50);
}

#endif
