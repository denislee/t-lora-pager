/**
 * @file      cc1101.cpp
 * @brief     CC1101 (sub-GHz FSK/OOK) — radio_chip:: driver (hal/radio_chip.h).
 */

#include "../../hal_interface.h"
#include "../radio_chip.h"

#ifdef ARDUINO_LILYGO_LORA_CC1101

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

#define RADIO_DEFAULT_BIT_RATE 38.4  // kbps
#define RADIO_DEFAULT_DEV_FREQ 20.0

namespace radio_chip {

void default_params(radio_params_t &params)
{
    params.bandwidth = 102.0;
    params.freq      = 433.0;
    params.cr        = 0;
    params.sf        = 0;
    params.power     = 10;
    params.syncWord  = 0xCD;
}

int16_t configure(const radio_params_t &params)
{
#ifdef ARDUINO
    int16_t state = radio.setFrequencyDeviation(params.freq);
    if (state == RADIOLIB_ERR_INVALID_FREQUENCY) {
        Serial.println(F("Selected frequency is invalid for this module!"));
    }
    state = radio.setRxBandwidth(params.bandwidth);
    if (state == RADIOLIB_ERR_INVALID_BANDWIDTH) {
        Serial.println(F("Selected bandwidth is invalid for this module!"));
    }
    state = radio.setSyncWord(params.syncWord, 0x23);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.println(F("Unable to set sync word!"));
    }
    state = radio.setOutputPower(params.power);
    if (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER) {
        Serial.println(F("Selected output power is invalid for this module!"));
    }
    state = radio.setBitRate(RADIO_DEFAULT_BIT_RATE);
    if (state == RADIOLIB_ERR_INVALID_BIT_RATE) {
        Serial.println(F("[CC1101] Selected bit rate is invalid for this module!"));
    }
    if (radio.setFrequencyDeviation(RADIO_DEFAULT_DEV_FREQ) == RADIOLIB_ERR_INVALID_FREQUENCY_DEVIATION) {
        Serial.println(F("[CC1101] Selected frequency deviation is invalid for this module!"));
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


#endif  // ARDUINO_LILYGO_LORA_CC1101
