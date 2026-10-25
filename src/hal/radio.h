/**
 * @file      radio.h
 * @brief     LoRa/FSK radio (sx126x/cc1101/sx128x/lr1121) enable + config.
 */
#pragma once

#include "types.h"

// --- Primary radio module (LoRa / FSK) ---
bool hw_get_radio_enable();
// Returns RADIOLIB_ERR_NONE (0) on success, or a negative RadioLib status
// code from radio_chip::configure() on failure. The persisted user_setting
// is updated regardless so the user's intent is remembered across boots even
// if the hardware rejected it this time.
int16_t hw_set_radio_enable(bool en);

