/**
 * @file      sx1262.cpp
 * @brief     SX1262 (sub-GHz LoRa) — radio_chip:: driver (hal/radio_chip.h).
 */

#include "../../hal_interface.h"
#include "../radio_chip.h"

#ifdef ARDUINO_LILYGO_LORA_SX1262

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

namespace radio_chip {

void default_params(radio_params_t &params)
{
    params.bandwidth = 125.0;
    params.freq      = RADIO_DEFAULT_FREQUENCY;
    params.cr        = 5;
    params.sf        = 12;
    params.power     = 22;
    params.syncWord  = 0xCD;
}

int16_t configure(const radio_params_t &params)
{
#ifdef ARDUINO
    // Wake the chip before touching any registers. RadioLib requires the chip
    // to be in standby (not sleep) before register writes succeed after a
    // prior radio.sleep() call, so configure() is self-healing if the chip
    // was sleeping.
    radio.standby();

    int16_t state = radio.setFrequency(params.freq);
    if (state == RADIOLIB_ERR_INVALID_FREQUENCY) {
        Serial.println(F("Selected frequency is invalid for this module!"));
    }
    state = radio.setBandwidth(params.bandwidth);
    if (state == RADIOLIB_ERR_INVALID_BANDWIDTH) {
        Serial.println(F("Selected bandwidth is invalid for this module!"));
    }
    state = radio.setSpreadingFactor(params.sf);
    if (state == RADIOLIB_ERR_INVALID_SPREADING_FACTOR) {
        Serial.println(F("Selected spreading factor is invalid for this module!"));
    }
    state = radio.setCodingRate(params.cr);
    if (state == RADIOLIB_ERR_INVALID_CODING_RATE) {
        Serial.println(F("Selected coding rate is invalid for this module!"));
    }
    state = radio.setSyncWord(params.syncWord);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.println(F("Unable to set sync word!"));
    }
    state = radio.setOutputPower(params.power);
    if (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER) {
        Serial.println(F("Selected output power is invalid for this module!"));
    }
    state = radio.setCurrentLimit(140);
    if (state == RADIOLIB_ERR_INVALID_CURRENT_LIMIT) {
        Serial.println(F("Selected current limit is invalid for this module!"));
    }

    return radio.sleep();
#else
    (void)params;
    return 0;
#endif
}

int16_t sleep()
{
#ifdef ARDUINO
    return radio.sleep();
#else
    return 0;
#endif
}

} // namespace radio_chip

#endif  // ARDUINO_LILYGO_LORA_SX1262
