/**
 * @file      lr1121.cpp
 * @brief     LR1121 (multi-band LoRa incl. 2.4 GHz) — radio_chip:: driver
 *            (hal/radio_chip.h). Spans sub-GHz and 2.4 GHz; the output power
 *            is capped at 13 dBm in the 2.4 GHz band.
 */

#include "../../hal_interface.h"
#include "../radio_chip.h"

#ifdef ARDUINO_LILYGO_LORA_LR1121

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
    // LR1121 needs a full re-init when crossing bands; simplest is to always
    // re-init here. Matches the original pre-refactor behavior.
    instance.initLoRa();

#ifdef ARDUINO_T_DECK_V2
    instance.setRFFrequencyBand(params.freq);
#endif

    int16_t state = radio.setFrequency(params.freq);
    if (state == RADIOLIB_ERR_INVALID_FREQUENCY) {
        Serial.println(F("Selected frequency is invalid for this module!"));
    }

    state = radio.setBandwidth(params.bandwidth);
    if (state == RADIOLIB_ERR_INVALID_BANDWIDTH) {
        state = radio.setBandwidth(params.bandwidth, true);
        if (state == RADIOLIB_ERR_INVALID_BANDWIDTH) {
            Serial.println(F("Selected bandwidth is invalid for this module!"));
        }
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

    // 2.4 GHz PA tops out at 13 dBm on this module; clamp silently.
    bool highFreq = false;
    uint8_t power = params.power;
    if (params.freq >= 2400 && power > 13) {
        power = 13;
        highFreq = true;
    }
    state = radio.setOutputPower(power, highFreq);
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


#endif  // ARDUINO_LILYGO_LORA_LR1121
