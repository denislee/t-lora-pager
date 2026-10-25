/**
 * @file      radio_chip.h
 * @brief     Private per-chip interface used by hal/radio.cpp.
 *
 * Exactly one driver in hal/radio/ (sx1262 / sx1280 / lr1121 / cc1101) is
 * selected at build time by ARDUINO_LILYGO_LORA_<chip> and implements the
 * functions below. Nothing transmits or receives: the user's Radio toggle
 * only programs the startup settings and parks the chip (P4.18, DC2.3.13).
 *
 * Not a public API — do not include from src/ui_*.cpp. Only hal/radio.cpp,
 * hal/system.cpp (sleep on fake-sleep entry) and the drivers include it.
 */
#pragma once

#include "types.h"

namespace radio_chip {

// Fill `p` with this chip's startup defaults (band, bandwidth, SF, CR, power,
// sync word) without touching the hardware.
void default_params(radio_params_t &p);

// Apply `p` to the hardware, then park the chip (sleep on the SX1262,
// standby on the others). Caller holds the SPI lock. Returns the status of
// the parking call, or of the last setter when parking is not reached.
int16_t configure(const radio_params_t &p);

// Put the chip into its lowest-power sleep state. Caller must hold the SPI
// lock. Returns RADIOLIB_ERR_NONE on success. After this call the chip must
// be woken (standby) before any register writes — configure() handles that
// via the top-of-function radio.standby() wake call.
int16_t sleep();

} // namespace radio_chip
