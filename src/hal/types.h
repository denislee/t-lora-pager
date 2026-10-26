/**
 * @file      types.h
 * @brief     Shared HAL data types, enums, and constants.
 *
 * All domain HAL headers (system, display, power, ...) include this file.
 * Keep it free of function declarations.
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <string>
#include <vector>

typedef enum {
    MEDIA_VOLUME_UP,
    MEDIA_VOLUME_DOWN,
    MEDIA_PLAY_PAUSE,
    MEDIA_NEXT,
    MEDIA_PREVIOUS
} media_key_value_t;

/* Radio frequency constants */
#define RADIO_DEFAULT_FREQUENCY  868.0

#ifndef ARDUINO

#ifndef _BV
#define _BV(x)                      (1UL<<x)
#endif

// Arduino's esp32-hal-log macros are unavailable on the emulator.
// Route to printf so log_i/log_e/log_w/log_d calls still produce output in
// terminal mode; tag prefixes mirror the ESP-IDF style so filters recognise
// them. The `##__VA_ARGS__` trick strips the trailing comma when the caller
// passes no format args.
#ifndef log_i
#define log_i(fmt, ...) fprintf(stdout, "[I] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef log_w
#define log_w(fmt, ...) fprintf(stderr, "[W] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef log_e
#define log_e(fmt, ...) fprintf(stderr, "[E] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef log_d
#define log_d(fmt, ...) ((void)0)
#endif

// Emulator-only fallback constants. The vendor LilyGo_LoRa_Pager.h defines
// the authoritative hardware values (e.g. DEVICE_MAX_BRIGHTNESS_LEVEL=16 for
// the AW9364 16-step driver). Wrap each in #ifndef so the vendor header always
// wins when present — include order must not matter.
#ifndef DEVICE_MAX_BRIGHTNESS_LEVEL
#define DEVICE_MAX_BRIGHTNESS_LEVEL 255
#endif
#ifndef DEVICE_MIN_BRIGHTNESS_LEVEL
#define DEVICE_MIN_BRIGHTNESS_LEVEL 0
#endif
#ifndef DEVICE_CHARGE_LEVEL_NUMS
#define DEVICE_CHARGE_LEVEL_NUMS    12
#endif
#ifndef DEVICE_CHARGE_STEPS
#define DEVICE_CHARGE_STEPS         1
#endif
#ifndef DEVICE_CHARGE_CURRENT_RECOMMEND
#define DEVICE_CHARGE_CURRENT_RECOMMEND 1000  // an hw_set_charger_current_level() step
#endif
#define USING_RADIO_NAME            "SX12XX"

// Hardware online status bit definitions
#define HW_RADIO_ONLINE             (_BV(0))
#define HW_TOUCH_ONLINE             (_BV(1))
#define HW_DRV_ONLINE               (_BV(2))
#define HW_PMU_ONLINE               (_BV(3))
#define HW_RTC_ONLINE               (_BV(4))
#define HW_PSRAM_ONLINE             (_BV(5))
#define HW_GPS_ONLINE               (_BV(6))
#define HW_SD_ONLINE                (_BV(7))
#define HW_NFC_ONLINE               (_BV(8))
#define HW_BHI260AP_ONLINE          (_BV(9))
#define HW_KEYBOARD_ONLINE          (_BV(10))
#define HW_GAUGE_ONLINE             (_BV(11))
#define HW_EXPAND_ONLINE            (_BV(12))
#define HW_CODEC_ONLINE             (_BV(13))
#define HW_NRF24_ONLINE             (_BV(14))
#define HW_BMA423_ONLINE            (_BV(18))

#else
#include <WiFi.h>
#endif

// Startup settings a radio_chip:: driver applies before parking the chip
// (hal/radio_chip.h). Nothing transmits or receives (P4.18 / DC2.3.13).
typedef struct {
    float freq;
    float bandwidth;
    uint16_t cr;
    uint8_t power;
    uint8_t sf;
    uint8_t syncWord;
} radio_params_t;

typedef struct {
    uint8_t authmode;
    int8_t  rssi;
    int32_t channel;
    std::string ssid;
} wifi_scan_params_t;

typedef struct {
    std::string ssid;
    std::string password;
} wifi_conn_params_t;

typedef struct {
    uint16_t battery_voltage;
    int      battery_percent;
    bool     is_charging;
} monitor_params_t;

typedef struct {
    uint8_t brightness_level;
    uint8_t keyboard_bl_level;
    uint8_t disp_timeout_second;
    uint16_t charger_current;
    uint8_t charger_enable;
    uint8_t sleep_mode;
    uint8_t editor_font_size;
    uint8_t editor_font_index;
    uint8_t journal_font_size;
    uint8_t journal_font_index;
    uint8_t charge_limit_en;
    uint8_t wifi_enable;
    uint8_t bt_enable;
    uint8_t radio_enable;
    uint8_t nfc_enable;
    uint8_t speaker_enable;
    uint8_t haptic_enable;
    uint8_t show_mem_usage;
    uint8_t show_file_count;
    uint16_t cpu_freq_mhz;
    uint8_t msc_prefer_sd;
    uint8_t prune_internal;
    uint8_t header_font_size;
    uint8_t header_font_index;
    uint8_t home_font_size;
    uint8_t home_font_index;
    uint8_t system_font_size;
    uint8_t system_font_index;
    uint8_t weather_font_size;
    uint8_t weather_font_index;
    uint8_t telegram_font_size;
    uint8_t telegram_font_index;
    uint8_t ssh_font_size;
    uint8_t ssh_font_index;
    uint8_t chat_font_size;
    uint8_t chat_font_index;
} user_setting_params_t;

typedef struct {
    const char *filename;
} audio_params_t;

// One Settings device-probe row (hw_get_devices()).
typedef struct {
    uint32_t    online_bit;  // HW_*_ONLINE
    const char *name;
} hw_device_t;

typedef struct {
    float roll;
    float pitch;
    float heading;
    uint8_t orientation;
} imu_params_t;

