/**
 * @file      sx1280.cpp
 * @brief     SX1280 (2.4 GHz LoRa) — radio_chip:: driver (hal/radio_chip.h).
 */

#include "../../hal_interface.h"
#include "../radio_chip.h"

#ifdef ARDUINO_LILYGO_LORA_SX1280

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

namespace radio_chip {

void default_params(radio_params_t &params)
{
    params.bandwidth = 203.125;
    params.freq      = 2400.0;
    params.cr        = 5;
    params.sf        = 12;
    params.power     = 13;
    params.syncWord  = 0xCD;
}

int16_t configure(const radio_params_t &params)
{
#ifdef ARDUINO
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

    return radio.standby();
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


#endif  // ARDUINO_LILYGO_LORA_SX1280
