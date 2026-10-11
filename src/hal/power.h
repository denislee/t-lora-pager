/**
 * @file      power.h
 * @brief     Battery, charger, OTG, and power monitoring.
 */
#pragma once

#include "types.h"

int16_t hw_get_battery_voltage();
void hw_update_battery_history();

void hw_get_monitor_params(monitor_params_t &params);

bool hw_get_otg_enable();
bool hw_set_otg(bool enable);
bool hw_has_otg_function();

bool hw_get_charge_enable();
void hw_set_charger(bool enable);
void hw_set_charger_current(uint16_t milliampere);

uint8_t hw_get_charge_level_nums();

uint16_t hw_set_charger_current_level(uint8_t level);
uint8_t hw_get_charger_current_level();
