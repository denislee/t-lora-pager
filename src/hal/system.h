/**
 * @file      system.h
 * @brief     System-level HAL: init, device info, power modes, settings, feedback.
 */
#pragma once

#include "types.h"

void hw_init();
// Applies the persisted WiFi/BT/LoRa/NFC enable toggles. Deliberately called
// AFTER the LVGL task starts (see factory.ino setup()) so radio/network stack
// bring-up (~0.5–1 s when enabled) doesn't block the first rendered frame.
void hw_connectivity_init();

uint16_t hw_get_devices_nums();
const char *hw_get_devices_name(int index);
bool hw_get_mac(uint8_t *mac);
uint32_t hw_get_device_online();
const char *hw_get_firmware_hash_string();
const char *hw_get_chip_id_string();
void hw_get_arduino_version(std::string &param);

void hw_get_date_time(std::string &param);
void hw_get_date_time(struct tm &timeinfo);
void hw_set_date_time(struct tm &timeinfo);

// Cheap wall-clock read for frequently-polled *display* use (status bar,
// glance overlay — once per second). Reads the ESP32 system clock via
// time()/localtime_r instead of hitting the external RTC over I2C, so it
// does not contend the instance mutex with the 10 ms keyboard task. The
// system clock is the correct source of truth here: hw_init() seeds it from
// the RTC at boot, and NTP/GPS/manual sets keep both in step. This firmware
// never deep-sleeps (fake/light sleep preserves the system clock), so the
// value stays valid across sleep. Use hw_get_date_time() when the RTC is the
// authority (settings, one-off timestamps).
void hw_get_wall_clock(struct tm &timeinfo);

// Non-blocking NTP sync. Starts SNTP (requires WiFi up) and returns
// immediately; poll hw_get_time_sync_status() to detect completion. The
// RTC write happens in factory.ino's SNTP notification callback, so no
// follow-up work is required from the caller beyond refreshing any UI that
// reflects the current time.
//
// `gmt_offset_sec` overrides the compile-time GMT_OFFSET_SECOND constant —
// pass INT_MIN (or call the no-arg overload) to use the default. Settings
// flows that let the user pick a timezone should pass the resolved offset
// here so the synced wall-clock time matches the user's chosen city.
bool hw_start_time_sync_ntp();
bool hw_start_time_sync_ntp(int gmt_offset_sec);
// 1 = synced since last start, 0 = not yet (in progress or reset).
int  hw_get_time_sync_status();
// Called from the SNTP notification callback (factory.ino) to mark the
// sync as completed. Decouples UI feedback from sntp_get_sync_status(),
// which in SNTP_SYNC_MODE_IMMED often stays at RESET even after a
// successful update.
void hw_notify_time_sync_completed();

void hw_shutdown();
void hw_sleep();
void hw_power_down_all();
void hw_power_up_all();
void hw_low_power_loop();

// P4.29 — call periodically from the charge task while in fake sleep.
// Locks the notes passphrase once the device has been continuously fake-sleeping
// for the configured grace period (5 min). now_ms should be millis().
// Idempotent: after locking it is a no-op until the next fake-sleep entry.
void hw_fake_sleep_tick(uint32_t now_ms);
void hw_feedback();
bool hw_get_haptic_enable();
void hw_set_haptic_enable(bool en);

void hw_get_user_setting(user_setting_params_t &param);
void hw_load_setting();
void hw_set_user_setting(user_setting_params_t &param);

void hw_get_heap_info(uint32_t &total, uint32_t &free);

// Tracked CPU-frequency setter — calls setCpuFrequencyMhz() only when the
// requested frequency differs from the last set value, eliminating redundant
// PLL re-locks across the hw_power_down_all / hw_power_up_all / loop() paths.
// No-op when mhz == 0.  On the emulator the tracker advances without calling
// the Arduino API (which doesn't exist there).
void     hw_set_cpu_freq(uint32_t mhz);
uint32_t hw_get_cpu_freq();

// P5.4 — the CPU frequency to hold while in fake sleep, resolved against the
// current radio state. Single owner for a rule that used to be duplicated
// (and could drift) between hw_power_down_all() and factory.ino's loop():
// BLE and WiFi both need >= 80 MHz, so return 80 while either link is up;
// otherwise return the deep idle floor. Cheap — two bool reads.
uint32_t hw_fake_sleep_target_freq();

// UI helper exposed from factory/ui_main for HAL-layer callers.
void ui_msg_pop_up(const char *title_txt, const char *msg_txt);
