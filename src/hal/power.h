/**
 * @file      power.h
 * @brief     Battery, charger, OTG, and power monitoring.
 */
#pragma once

#include "types.h"
#include <vector>

int16_t hw_get_battery_voltage();
// Enforce the 80 % charge limit (or plain charger_enable when it is off).
void hw_apply_charge_limit();

void hw_get_monitor_params(monitor_params_t &params);

bool hw_get_otg_enable();
bool hw_set_otg(bool enable);
bool hw_has_otg_function();

void hw_set_charger(bool enable);
void hw_set_charger_current(uint16_t milliampere);

uint8_t hw_get_charge_level_nums();

uint16_t hw_set_charger_current_level(uint8_t level);
uint8_t hw_get_charger_current_level();

// Milliamps of Settings levels 1..hw_get_charge_level_nums(), deduped.
void hw_charge_current_options(std::vector<uint16_t> &ma);
// First Settings level whose mA value equals ma, or 0 when none matches.
uint8_t hw_charge_level_for_ma(uint16_t ma);
