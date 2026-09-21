/**
 * @file      system.cpp
 * @brief     System init, settings persistence, device info, sleep/shutdown,
 *            date/time, feedback, and the shared HAL state definitions.
 */
#include "../hal_interface.h"
#include "internal.h"
#include "notes_crypto.h"
#include "sensors.h"
#include "keyboard_task.h"
#include "radio_chip.h"
#include "../core/spi_lock.h"

#include <cstring>
#include <lvgl.h>

using std::string;


#define NVS_NAME    "pager"
user_setting_params_t user_setting;

// Tracks the last frequency passed to setCpuFrequencyMhz() so hw_set_cpu_freq()
// can skip redundant PLL re-locks.  0 = unknown (forces a real set on first call).
static uint32_t s_cpu_freq_mhz = 0;

// P4.29 — notes passphrase lock grace period.
// We do NOT lock on every fake-sleep entry (that would demand a re-enter every
// 60 s once P4.1 is active). Instead we lock once the device has been
// continuously in fake sleep for NOTES_LOCK_GRACE_MS. The charge task drives
// the aging — it is the only thing still ticking in fake sleep — by calling
// hw_fake_sleep_tick() on each of its iterations.
// s_fake_sleep_entry_ms == 0 means "not in fake sleep / already locked this cycle".
static constexpr uint32_t NOTES_LOCK_GRACE_MS = 5u * 60u * 1000u; // 5 minutes
static uint32_t s_fake_sleep_entry_ms = 0;

// P4.22 — save the IMU registration state across a fake-sleep/wake cycle so
// hw_power_up_all() can restore it if the IMU debug page was open before sleep.
static bool s_imu_was_registered = false;

void hw_set_cpu_freq(uint32_t mhz)
{
    if (mhz == 0 || mhz == s_cpu_freq_mhz) return;
#ifdef ARDUINO
    setCpuFrequencyMhz(mhz);
#endif
    s_cpu_freq_mhz = mhz;
}

uint32_t hw_get_cpu_freq()
{
    return s_cpu_freq_mhz;
}

// P5.4 — deep idle floor for fake sleep, down from the previous 40 MHz.
//
// On the ESP32-S3 any frequency below 80 MHz already runs the CPU straight off
// the 40 MHz XTAL with the PLL powered down; 20 MHz is simply XTAL/2 and halves
// core dynamic power again on top of that. It is only selected when neither BLE
// nor WiFi is associated, so the >= 80 MHz requirement of both stacks is never
// violated, and by then every other consumer is parked: LVGL, keyboard and
// rotary tasks are blocked, the radio is asleep and the display is off.
//
// The one real consequence is that APB tracks the CPU when the PLL is off, so
// peripheral clocks divide with it — a bus configured for 400 kHz I2C runs near
// 100 kHz here. That is bounded and harmless: the charge task's VBUS read is the
// only periodic bus traffic left in this state, and hw_power_up_all() restores
// the user frequency before anything throughput-sensitive runs again.
//
// ⚠️ Bench note: this is the item in the P5 batch most worth a meter. If the
// measured delta from 40 MHz is noise, or anything misbehaves at XTAL/2, revert
// by setting this constant back to 40 — nothing else needs to change.
static constexpr uint32_t FAKE_SLEEP_IDLE_FREQ_MHZ = 20;

uint32_t hw_fake_sleep_target_freq()
{
    bool link_up = hw_get_ble_kb_connected() || hw_get_wifi_connected();
    return link_up ? 80 : FAKE_SLEEP_IDLE_FREQ_MHZ;
}

// Settings blob schema guard.
//
// The stored NVS blob is a `SettingsHeader` followed by a raw
// `user_setting_params_t`. The header lets us reject bytes left over from a
// previous schema instead of silently re-interpreting them as the new one.
//
// *** Bump SETTINGS_VERSION whenever `user_setting_params_t` changes in a way
//     that is not byte-compatible (field reorder, type change, field removed).
//     Adding a new field to the *end* keeps the size check effective and is
//     safe without a version bump — missing bytes keep their default values.
static constexpr uint32_t SETTINGS_MAGIC   = 0x50414752u; // "PAGR"
// v11: dropped user_setting_params_t.led_indicator_level (removed LED slider).
// A struct-layout change: the size guard in load already forces a clean
// defaults-reset on upgrade, and the version bump makes that explicit.
// v12: dropped user_setting_params_t.gps_enable — GPS has no continuous
// consumer, so the persisted "keep the rail on" setting just wasted power
// for nothing; GPS power is now purely transient, owned by
// hw_start_time_sync_gps() (see hal/gps_time_sync.cpp).
// v13: raised disp_timeout_second default from 0 ("never") to 60 s, and
// brightness_level default from 50 to 8 (half of the pager's 16-step AW9364
// clamp), so the PB auto-sleep and dim-stage savings actually activate out of
// the box.  The header-mismatch path wholesale-resets to defaults, which is
// the correct behaviour for both fields.
static constexpr uint16_t SETTINGS_VERSION = 13;

struct SettingsHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size;
};

#ifdef ARDUINO

#include "Esp.h"
#include <LilyGoLib.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <Preferences.h>
#include "driver/rtc_io.h"
#include "cJSON.h"

static Preferences           prefs;

void save_user_setting_nvs()
{
    uint8_t buf[sizeof(SettingsHeader) + sizeof(user_setting_params_t)];
    SettingsHeader h{SETTINGS_MAGIC, SETTINGS_VERSION, (uint16_t)sizeof(user_setting_params_t)};
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), &user_setting, sizeof(user_setting_params_t));
    prefs.putBytes(NVS_NAME, buf, sizeof(buf));
}

#endif

device_const_var_t dev_conts_var = {
    .max_brightness = DEVICE_MAX_BRIGHTNESS_LEVEL,
    .min_brightness = DEVICE_MIN_BRIGHTNESS_LEVEL,
    .max_charge_current = DEVICE_MAX_CHARGE_CURRENT,
    .min_charge_current = DEVICE_MIN_CHARGE_CURRENT,
    .charge_level_nums = DEVICE_CHARGE_LEVEL_NUMS,
    .charge_steps = DEVICE_CHARGE_STEPS,
};


static const char *hw_devices[] = {
    USING_RADIO_NAME,

#ifdef USING_INPUT_DEV_TOUCHPAD
    "Touch Panel",
#else
    "",
#endif
    "Haptic Drive",
    "Power management",
    "Real-time clock",
    "PSRAM",
    "GPS",
#ifdef HAS_SD_CARD_SOCKET
    "SD card",
#else
    "",
#endif
#ifdef USING_ST25R3916
    "NFC",
#else
    "",
#endif

#ifdef USING_BHI260_SENSOR
    "BHI260AP 6-Axis Sensor",
#else
    "",
#endif
#ifdef USING_INPUT_DEV_KEYBOARD
    "Keyboard",
#else
    "",
#endif

#ifdef USING_BQ_GAUGE
    "Gauge",
#else
    "",
#endif

#ifdef USING_XL9555_EXPANDS
    "Expands Control",
#else
    "",
#endif

#ifdef USING_AUDIO_CODEC
    "Audio codec",
#else
    "",
#endif

#ifdef USING_EXTERN_NRF2401
    "NRF2401 Sub 1G",
#else
    "",
#endif

#ifdef USING_SI473X_RADIO
    "SI4735 Radio",
#else
    "",
#endif

#ifdef USING_BME280
    "BME280 Pressure & Temperature",
#else
    "",
#endif

#ifdef USING_MAG_QMC5883
    "QMC5883P Magnetometer",
#else
    "",
#endif

#ifdef USING_BMA423_SENSOR
    "BMA423 Accelerometer",
#else
    "",
#endif

#ifdef USING_QMI8658_SENSOR
    "QMI8658 6-Axis Sensor",
#else
    "",
#endif

};


#ifndef ARDUINO
static time_t emu_time_offset = 0;

int random(int min, int max)
{
    if (min > max) {
        int temp = min;
        min = max;
        max = temp;
    }
    int range = max - min + 1;
    return rand() % range + min;
}
#endif


#ifdef ARDUINO

size_t getArduinoLoopTaskStackSize(void)
{
    // Measured on-device (OPTIMIZATION_PHASE3.md P3.7): loopTask's high-water
    // mark holds steady at 27,572 B free out of the old 30 KB (i.e. ~3.1 KB
    // ever used) across multiple NTP-retry cycles on a warm, WiFi-connected
    // system. Sized at measured-usage x1.5, rounded up to the nearest 4 KB.
    return 8 * 1024;
}

#endif






#ifdef ARDUINO_T_LORA_PAGER
const uint8_t mic_gain = 10;
#else
const uint8_t mic_gain = 10;
#endif


void hw_init()
{
#ifdef ARDUINO
    // P2.4: route all cJSON allocations to PSRAM. cJSON mints one small node
    // plus duplicated strings per JSON element, and the Arduino-ESP32 unified
    // allocator forces small allocations into internal DRAM — so a ~5 KB
    // Open-Meteo / Telegram payload expands to ~15-30 KB of internal heap, and
    // those spikes land on the network workers *while WiFi/TLS buffers are
    // live*, i.e. at peak internal-heap pressure. JSON parsing is not a hot
    // loop here, so PSRAM's higher latency is irrelevant. Prefer PSRAM but fall
    // back to internal DRAM if PSRAM is momentarily exhausted, so a parse never
    // fails outright; free() is heap-agnostic in the unified allocator, so one
    // free hook releases either. Every parse site cJSON_Delete()s before
    // returning, so nothing long-lived changes residence. Must run before any
    // app parses JSON — hw_init() is called at boot ahead of every app.
    // cJSON_InitHooks copies these two function pointers into its own globals,
    // so the struct itself need not outlive the call. The lambda is captureless
    // and decays to a plain function pointer.
    cJSON_Hooks cjson_psram_hooks = {
        [](size_t sz) -> void * {
            void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
            return p ? p : heap_caps_malloc(sz, MALLOC_CAP_8BIT);
        },
        free,
    };
    cJSON_InitHooks(&cjson_psram_hooks);

    if (instance.getDeviceProbe() & HW_RTC_ONLINE) {
        struct tm timeinfo;
        instance.rtc.getDateTime(&timeinfo);
        struct timeval tv;
        tv.tv_sec = mktime(&timeinfo);
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
        log_i("System time synchronized with RTC");
    }

    hw_audio_init();

    hw_radio_begin();
#ifdef USING_EXTERN_NRF2401
    hw_nrf24_begin();
#endif


#ifdef USING_AUDIO_CODEC
    if (HW_CODEC_ONLINE & hw_get_device_online()) {
        instance.codec.setVolume(100);
        instance.codec.setGain(mic_gain);
    } else {
        log_w("Audio codec not online!");
    }
#endif //USING_AUDIO_CODEC

#ifdef USING_INPUT_DEV_KEYBOARD
    instance.attachKeyboardFeedback(false, 0);

    instance.setFeedbackCallback([](void *args) {
        // Feedback disabled
    });
#endif //USING_INPUT_DEV_KEYBOARD


#endif

#ifdef ARDUINO
    hw_set_charger(user_setting.charger_enable);
    hw_set_charger_current(user_setting.charger_current);
    instance.setMSCPreferSD(user_setting.msc_prefer_sd != 0);
    // GPS has no persisted on/off setting or continuous consumer — force the
    // rail off at boot (the vendor's begin() defaults it HIGH as part of
    // expander GPIO setup). Only hw_start_time_sync_gps() powers it, and
    // only transiently, for the duration of a sync attempt.
    hw_set_gps_powered(false);
    hw_set_speaker_enable(user_setting.speaker_enable);
    hw_set_haptic_enable(user_setting.haptic_enable);
#endif

    // Battery history recording every 1 minute (60,000 ms)
    lv_timer_create(battery_history_timer_cb, 60000, NULL);
    // Record first point immediately - This also triggers the 80% charge limit check
    hw_update_battery_history();

#ifdef ARDUINO
    hw_set_disp_backlight(user_setting.brightness_level);

    hw_set_kb_backlight(user_setting.keyboard_bl_level);

    instance.onEvent([](DeviceEvent_t event, void *params, void *user_data) {
        int pmu_event = instance.getPMUEventType(params);
        if (pmu_event == PMU_EVENT_KEY_CLICKED) {
            log_d("ON EVENT PMU CLICK");
        } else if (pmu_event == PMU_EVENT_KEY_LONG_PRESSED) {
            log_d("ON EVENT PMU LONG CLICK -> Sleep");
            hw_low_power_loop();
        }
    }, POWER_EVENT, NULL);

    // Drive the keyboard from a dedicated high-priority task so it stays
    // responsive even when the main loop is held up by radio / NFC /
    // display flush / WiFi work. Must run after beginLvglHelper() so the
    // LVGL keypad indev exists and our read_cb can be swapped in.
    hw_keyboard_task_start();
#endif

}

// Applies the four connectivity toggles from the already-loaded user_setting.
// Deliberately called from setup() AFTER hw_lvgl_task_start() — WiFi/BLE/LoRa/
// NFC stack bring-up takes ~0.5–1 s each when enabled and used to block the
// first frame when it ran inside hw_init(). Late start is safe: all four are
// runtime-toggleable from Settings already, and the NFC task's poll is a no-op
// until discovery starts.
void hw_connectivity_init()
{
#ifdef ARDUINO
    hw_set_wifi_enable(user_setting.wifi_enable);
    hw_set_bt_enable(user_setting.bt_enable);
    hw_set_radio_enable(user_setting.radio_enable);
    hw_set_nfc_enable(user_setting.nfc_enable);
#endif
}

void hw_load_setting()
{
#ifdef ARDUINO
    // Initialize with defaults first
    user_setting.brightness_level = 8;  // P4.1/P4.3: half of the pager's 16-step AW9364 clamp
    user_setting.keyboard_bl_level = 80;
    user_setting.disp_timeout_second = 60; // P4.1: 60 s enables auto-sleep out of the box
    user_setting.charger_current = DEVICE_CHARGE_CURRENT_RECOMMEND;
    user_setting.charger_enable = true;
    user_setting.sleep_mode = 0;
    user_setting.editor_font_size = 14;
    user_setting.editor_font_index = 0;
    user_setting.journal_font_size = 14;
    user_setting.journal_font_index = 4; // Inter
    user_setting.header_font_size = 14;
    user_setting.header_font_index = 0;  // Montserrat
    user_setting.home_font_size = 16;
    user_setting.home_font_index = 0;    // Montserrat
    user_setting.system_font_size = 14;
    user_setting.system_font_index = 0;  // Montserrat
    user_setting.weather_font_size = 14;
    user_setting.weather_font_index = 4; // Inter — Latin-1 glyphs for diacritics
    user_setting.telegram_font_size = 14;
    user_setting.telegram_font_index = 4; // Inter — needed for Portuguese, Spanish, French accents
    user_setting.ssh_font_size = 10;
    user_setting.ssh_font_index = 0;     // Montserrat — packs more content per line; user can switch to a mono face
    user_setting.chat_font_size = 14;
    user_setting.chat_font_index = 0;    // Montserrat
    user_setting.charge_limit_en = false;
    user_setting.wifi_enable = 0;
    user_setting.bt_enable = 0;
    user_setting.radio_enable = 0;
    user_setting.nfc_enable = 0;
    user_setting.speaker_enable = 0;
    user_setting.haptic_enable = 1;
    user_setting.show_mem_usage = 0;
    user_setting.show_file_count = 0;
    user_setting.cpu_freq_mhz = 240;
    user_setting.storage_prefer_sd = 0;
    user_setting.msc_prefer_sd = 1;
    user_setting.prune_internal = 0;

    prefs.begin(NVS_NAME);
    const size_t stored_size = prefs.getBytesLength(NVS_NAME);
    constexpr size_t kVersionedSize = sizeof(SettingsHeader) + sizeof(user_setting_params_t);

    bool needs_save = true;

    if (stored_size == kVersionedSize) {
        uint8_t buf[kVersionedSize];
        prefs.getBytes(NVS_NAME, buf, sizeof(buf));
        const SettingsHeader *h = reinterpret_cast<const SettingsHeader *>(buf);
        if (h->magic == SETTINGS_MAGIC &&
            h->version == SETTINGS_VERSION &&
            h->payload_size == sizeof(user_setting_params_t)) {
            memcpy(&user_setting, buf + sizeof(SettingsHeader), sizeof(user_setting_params_t));
            // Fast path: the stored blob is already valid current-format, so
            // there is nothing to rewrite.
            needs_save = false;
        } else {
            log_i("Settings header mismatch (magic=%08x ver=%u size=%u), resetting to defaults",
                  (unsigned)h->magic, (unsigned)h->version, (unsigned)h->payload_size);
        }
    } else if (stored_size > 0 && stored_size <= sizeof(user_setting_params_t)) {
        // Legacy unversioned blob: copy what fits onto defaults, then re-save with header.
        prefs.getBytes(NVS_NAME, &user_setting, stored_size);
        log_i("Migrating legacy settings blob (%u bytes) to versioned format", (unsigned)stored_size);
    } else if (stored_size != 0) {
        log_i("Stored settings size %u unrecognized, resetting to defaults", (unsigned)stored_size);
    } else {
        log_i("No stored settings found, using defaults");
    }
    // Persist only when the stored blob wasn't already valid current-format
    // (legacy migration, header mismatch, unrecognized size, or nothing
    // stored) so a normal boot performs no redundant NVS flash write.
    if (needs_save) {
        save_user_setting_nvs();
    }
#else
    user_setting.brightness_level = 8;   // P4.1/P4.3: match hardware default
    user_setting.keyboard_bl_level = 255;
    user_setting.disp_timeout_second = 60; // P4.1: 60 s enables auto-sleep out of the box
    user_setting.charger_current = 1000;
    user_setting.charger_enable = true;
    user_setting.nfc_enable = 0;
    user_setting.speaker_enable = 0;
    user_setting.haptic_enable = 1;
#endif
}

void hw_get_user_setting(user_setting_params_t &param)
{
    param = user_setting;
}

void hw_set_user_setting(user_setting_params_t &param)
{
    user_setting = param;
#ifdef ARDUINO
    save_user_setting_nvs();
#endif
}



bool hw_get_haptic_enable() { return user_setting.haptic_enable; }
void hw_set_haptic_enable(bool en) {
    user_setting.haptic_enable = en;
#ifdef ARDUINO
    instance.powerControl(POWER_HAPTIC_DRIVER, en);
    delay(10);
#endif
}

uint16_t hw_get_devices_nums()
{
    return sizeof(hw_devices) / sizeof(hw_devices[0]);
}

const char *hw_get_devices_name(int index)
{
    if (index >= hw_get_devices_nums()) {
        return "NULL";
    }
    return hw_devices[index];
}

bool hw_get_mac(uint8_t *mac)
{
#ifdef ARDUINO
    esp_efuse_mac_get_default(mac);
    return true;
#endif
    return false;
}

void hw_get_date_time(string &param)
{
#ifdef ARDUINO
    struct tm timeinfo;
    if (hw_get_device_online() & HW_RTC_ONLINE) {
        instance.rtc.getDateTime(&timeinfo);
        char datetime[128] = {0};
        snprintf(datetime, 128, "%04d/%02d/%02d %02d:%02d:%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        param  = datetime;
    } else {
        param = "2000/01/01 00:00:00";
    }
#else
    time_t now;
    struct tm *timeinfo;
    time(&now);
    now += emu_time_offset;
    timeinfo = localtime(&now);
    char datetime[128] = {0};
    snprintf(datetime, 128, "%04d/%02d/%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900,
             timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour,
             timeinfo->tm_min,
             timeinfo->tm_sec);
    param  = datetime;
#endif
}

void hw_get_date_time(struct tm &timeinfo)
{
#ifdef ARDUINO
    if (hw_get_device_online() & HW_RTC_ONLINE) {
        instance.rtc.getDateTime(&timeinfo);
    } else {
        time_t now;
        time(&now);
        localtime_r(&now, &timeinfo);
    }
#else
    time_t now;
    time(&now);
    now += emu_time_offset;
    timeinfo = *localtime(&now);
#endif
}

void hw_get_wall_clock(struct tm &timeinfo)
{
    // See system.h: read the system clock, not the RTC over I2C. Same
    // localtime_r fallback that hw_get_date_time uses when the RTC is
    // offline — but here it is the primary path, and it holds the instance
    // mutex for zero time.
    time_t now;
    time(&now);
#ifndef ARDUINO
    now += emu_time_offset;
#endif
    localtime_r(&now, &timeinfo);
}

void hw_set_date_time(struct tm &timeinfo)
{
#ifdef ARDUINO
    struct timeval tv;
    tv.tv_sec = mktime(&timeinfo);
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    if (hw_get_device_online() & HW_RTC_ONLINE) {
        instance.rtc.setDateTime(timeinfo);
        instance.rtc.hwClockWrite();
    }
#else
    time_t target = mktime(&timeinfo);
    time_t now;
    time(&now);
    emu_time_offset = target - now;
#endif
}

// --- NTP sync -------------------------------------------------------------
// Manual-trigger counterpart to the automatic sync done from
// factory.ino::WiFiGotIP(). Starts SNTP and returns immediately so the UI
// stays responsive; the caller polls hw_get_time_sync_status() on an
// lv_timer to learn when it's done. The actual RTC write happens in the
// factory.ino time_available() callback that esp_sntp invokes on success.
//
// Calling configTime() after the DHCP flow has already set NTP via option
// 42 is safe — it overrides whatever the DHCP server handed us.
// Flipped by the SNTP notification callback in factory.ino via
// hw_notify_time_sync_completed(). More reliable than sntp_get_sync_status(),
// which in the default SNTP_SYNC_MODE_IMMED often stays at RESET even after
// a successful update.
static volatile bool g_ntp_sync_completed = false;

bool hw_start_time_sync_ntp(int gmt_offset_sec)
{
#ifdef ARDUINO
    if (!hw_get_wifi_connected()) return false;
    g_ntp_sync_completed = false;
    // Force a fresh sync cycle: if SNTP was already COMPLETED from boot,
    // reset it so the callback fires again on the next successful update.
    sntp_stop();
    configTime(gmt_offset_sec, 0, "pool.ntp.org", "time.nist.gov");
    return true;
#else
    (void)gmt_offset_sec;
    return false;
#endif
}

bool hw_start_time_sync_ntp()
{
    return hw_start_time_sync_ntp((int)GMT_OFFSET_SECOND);
}

void hw_notify_time_sync_completed()
{
    g_ntp_sync_completed = true;
}

// 1 = synced since last start, 0 = not yet (in progress or reset).
int hw_get_time_sync_status()
{
#ifdef ARDUINO
    if (g_ntp_sync_completed) return 1;
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) return 1;
    return 0;
#else
    return -1;
#endif
}



void hw_get_arduino_version(string &param)
{
#ifdef ARDUINO
    param.clear();
    param.append("V");
    param.append(std::to_string(ESP_ARDUINO_VERSION_MAJOR));
    param.append(".");
    param.append(std::to_string(ESP_ARDUINO_VERSION_MINOR));
    param.append(".");
    param.append(std::to_string(ESP_ARDUINO_VERSION_PATCH));
#else
    param = "V2.0.17";
#endif
}


uint32_t hw_get_device_online()
{
#ifdef ARDUINO
    return instance.getDeviceProbe();
#else
    uint32_t hw_online =   HW_TOUCH_ONLINE | HW_DRV_ONLINE | HW_PMU_ONLINE;
#ifdef USING_INPUT_DEV_KEYBOARD
    hw_online |= HW_KEYBOARD_ONLINE;
#endif
    return hw_online;
#endif
}





void hw_shutdown()
{
#ifdef ARDUINO
    instance.decrementBrightness(0, 5, false);
#if defined(USING_PPM_MANAGE)
    instance.ppm.shutdown();
#elif defined(USING_PMU_MANAGE)
    instance.pmu.shutdown();
#endif
#endif
}

void hw_power_down_all()
{
#ifdef ARDUINO
    // Disable BQ25896 continuous ADC measurement during fake sleep — nobody
    // reads the gauge while LVGL is blocked, so the ~0.5–1 mA conversion
    // current is pure waste.  Mirrors vendor lightSleep() (LilyGo_LoRa_Pager.cpp:685).
#if defined(USING_PPM_MANAGE)
    instance.ppm.disableMeasure();
#endif
    instance.powerControl(POWER_GPS, false);
    instance.powerControl(POWER_NFC, false);
    instance.powerControl(POWER_HAPTIC_DRIVER, false);
    instance.powerControl(POWER_SPEAK, false);
    instance.powerControl(POWER_KEYBOARD, false);
    hw_disable_keyboard();
    // SD Card is left on to avoid mount/unmount overhead and potential filesystem issues

    // Lower CPU frequency for power saving during fake sleep. BLE and WiFi
    // both need ≥80MHz — dropping below that while either link is up severs
    // it, so hw_fake_sleep_target_freq() holds at 80MHz in that case and
    // otherwise returns the deep idle floor (P5.4).
    hw_set_cpu_freq(hw_fake_sleep_target_freq());

    // Sleep the radio chip — vendor lightSleep() parity (LilyGo_LoRa_Pager.cpp:687).
    // STANDBY_RC draws ~0.6–1.6 mA; sleep is <1 µA. SPI lock required because
    // radio_chip::sleep() bypasses hw_set_radio_params() and its internal lock.
    // Does NOT touch user_setting.radio_enable — intent is preserved across wake.
    {
        core::ScopedSpiLock spi;
        radio_chip::sleep();
    }

    // PB.2 — escalate WiFi power-save to MAX_MODEM during fake sleep.
    // LVGL timers (incl. the 60 s Telegram poll) are frozen while fake-sleeping,
    // so there is no inbound-latency consumer — MAX_MODEM lets the RF front-end
    // sleep for the listen interval, saving ~1–5 mA while associated.
    // No-op when WiFi is disconnected.
    hw_wifi_powersave_sleep();

    // Suspend the three BHI260 virtual sensors (ACCEL_PASSTHROUGH, GAME_ROTATION_VECTOR,
    // DEVICE_ORIENTATION) only if they are currently streaming. During fake sleep the
    // LVGL task is blocked so sensor.update() never runs and the BHI260 FIFO just fills
    // forever — pure waste (~0.5–1 mA on I2C + sensor compute). BHI260 is I2C, not SPI,
    // so no ScopedSpiLock needed here. Save state so power_up_all() can restore it.
    // (P4.22: IMU is only registered on demand by the debug page, not at boot.)
    s_imu_was_registered = hw_imu_is_registered();
    if (s_imu_was_registered) hw_unregister_imu_process();

    // P4.29 — record when we entered fake sleep so hw_fake_sleep_tick() can
    // lock the notes passphrase after the grace period.
    s_fake_sleep_entry_ms = millis();
#endif
}

void hw_power_up_all()
{
#ifdef ARDUINO
    // Revert CPU frequency to user set value
    hw_set_cpu_freq(user_setting.cpu_freq_mhz);

    // GPS is intentionally not restored here: it has no persisted enable
    // setting and no continuous consumer, so it stays off across fake sleep
    // and is only ever powered transiently by hw_start_time_sync_gps().
    if (user_setting.nfc_enable) instance.powerControl(POWER_NFC, true);
    if (user_setting.haptic_enable) instance.powerControl(POWER_HAPTIC_DRIVER, true);
    instance.powerControl(POWER_KEYBOARD, true);
    hw_enable_keyboard();
    // Re-enable BQ25896 continuous ADC measurement so the battery gauge is
    // live again after wake.  Mirrors vendor lightSleep() (LilyGo_LoRa_Pager.cpp:756).
#if defined(USING_PPM_MANAGE)
    instance.ppm.enableMeasure();
#endif
    // Speaker is only powered on when needed by playback routines and if enabled

    // Restore the radio to the user-configured state after the sleep placed in
    // hw_power_down_all(). configure()'s top-of-function radio.standby() self-heals
    // the chip from sleep before re-applying settings. hw_set_radio_enable →
    // hw_set_radio_params already takes ScopedSpiLock internally — no extra lock here.
    hw_set_radio_enable(user_setting.radio_enable);

    // PB.2 — restore WiFi power-save to MIN_MODEM after fake sleep.
    // Reverts the MAX_MODEM escalation applied in hw_power_down_all() so normal
    // beacon-wakeup cadence resumes for any outbound requests (hub probe, etc.).
    // No-op when WiFi is disconnected.
    hw_wifi_powersave_active();

    // Re-register the BHI260 virtual sensors only if they were active before the
    // fake sleep (P4.22: IMU is registered on demand by the debug page, not at boot).
    // BHI260 is I2C, not SPI, so no ScopedSpiLock needed here.
    if (s_imu_was_registered) hw_register_imu_process();
    s_imu_was_registered = false;

    // P4.29 — reset the grace-period timer on wake so the next fake-sleep
    // cycle gets a fresh 5-minute window.
    s_fake_sleep_entry_ms = 0;
#endif
}

// P4.29 — notes passphrase grace-period aging.
// Called periodically by the charge task while the device is in fake sleep.
// Locks the notes passphrase once the device has been continuously fake-sleeping
// for NOTES_LOCK_GRACE_MS (5 min) without waking. Idempotent after locking:
// once s_fake_sleep_entry_ms is cleared here, subsequent ticks are no-ops.
void hw_fake_sleep_tick(uint32_t now_ms)
{
#ifdef ARDUINO
    if (s_fake_sleep_entry_ms == 0) return; // not in fake sleep / already locked
    if ((now_ms - s_fake_sleep_entry_ms) >= NOTES_LOCK_GRACE_MS) {
        notes_crypto_lock();
        s_fake_sleep_entry_ms = 0; // prevent repeated calls to notes_crypto_lock()
    }
#else
    (void)now_ms;
#endif
}

void hw_sleep()
{
#ifdef ARDUINO
    /* Drop the in-RAM passphrase before suspending — a device found asleep
     * should be indistinguishable from one that's been off. */
    notes_crypto_lock();
    hw_audio_deinit_task();

#ifdef USING_PDM_MICROPHONE
    instance.mic.end();
#endif

#ifdef USING_PCM_AMPLIFIER
    instance.player.end();
#endif

    instance.decrementBrightness(0, 5, false);
    instance.sleep((WakeupSource_t)(WAKEUP_SRC_BOOT_BUTTON));
#endif
}

void hw_feedback()
{
#ifdef ARDUINO
    instance.vibrator();
#endif
}

void hw_low_power_loop()
{
#ifdef ARDUINO
    notes_crypto_lock();
    if (user_setting.sleep_mode == 1) {
        hw_sleep();
    } else {
        instance.lightSleep((WakeupSource_t)(WAKEUP_SRC_BOOT_BUTTON));
    }
#endif
}

#if defined(ARDUINO)
#include <Esp.h>
#endif

void hw_get_heap_info(uint32_t &total, uint32_t &free)
{
#if defined(ARDUINO)
    total = ESP.getHeapSize();
    free = ESP.getFreeHeap();
#else
    total = 512 * 1024;
    free = 256 * 1024;
#endif
}




const char *hw_get_firmware_hash_string()
{
#ifdef ARDUINO
    static char hash_string[33] = {0};
    snprintf(hash_string, sizeof(hash_string), "%s", ESP.getSketchMD5().c_str());
    return hash_string;
#else
    return "DummyHashString";
#endif
}

const char *hw_get_chip_id_string()
{
#ifdef ARDUINO
    static char chipid[13] = {0};
    uint64_t chipmacid = 0LL;
    esp_efuse_mac_get_default((uint8_t *)(&chipmacid));
    snprintf(chipid, sizeof(chipid), "%04X%08X", (uint16_t)(chipmacid >> 32), (uint32_t)(chipmacid));
    return chipid;
#endif
    return "DummyChipIDString";
}


