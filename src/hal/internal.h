/**
 * @file      internal.h
 * @brief     HAL implementation internals shared across hal/*.cpp files.
 *
 * Not a public API. Do not include from src/ui_*.cpp or src/apps/.
 */
#pragma once

#include "types.h"

typedef struct _device_const_var {
    uint16_t max_brightness;
    uint16_t min_brightness;
    uint8_t  charge_level_nums;
    uint8_t  charge_steps;
} device_const_var_t;

extern user_setting_params_t user_setting;
extern device_const_var_t dev_conts_var;

#include <lvgl.h>
void battery_history_timer_cb(lv_timer_t *timer);

void hw_audio_init();
void hw_audio_deinit_task();

void save_user_setting_nvs();

// Boot-time radio init. `hw_radio_begin()` lives in hal/radio_common.cpp
// (one-time attach of the LoRa/FSK ISR); `hw_nrf24_begin()` lives in the
// per-chip file hal/radio/nrf2401.cpp (NRF24 IRQ attach). Called once from
// hal/system.cpp's hw_init().
void hw_radio_begin();
void hw_nrf24_begin();

#ifndef ARDUINO
int random(int min, int max);
#endif
