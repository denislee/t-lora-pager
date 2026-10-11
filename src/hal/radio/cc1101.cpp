/**
 * @file      hw_cc1101.cpp
 * @brief     CC1101 (sub-GHz FSK/OOK) — per-chip programming.
 *
 * Shared ISR/event-group/TX/RX plumbing lives in hal/radio_common.cpp.
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
    params.mode      = RADIO_DISABLE;
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

    switch (params.mode) {
    case RADIO_DISABLE: state = radio.standby();         break;
    case RADIO_TX:      state = radio.startTransmit(""); break;
    case RADIO_RX:      state = radio.startReceive();    break;
    case RADIO_CW:                                       break;
    default:                                             break;
    }
    return state;
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


// ----- Option tables -----

static const float bandwidth_list[]   = {0.025, 5, 10, 20, 30, 60, 80, 100, 120, 150, 200, 300, 400, 500, 600};
static const float power_level_list[] = {-30, -20, -15, -10, 0, 5, 7, 10};
static const float freq_list[]        = {
    387.0, 400.0, 410.0, 420.0, 433.0, 440.0, 450.0, 460.0, 464.0
};

uint16_t radio_get_freq_length()      { return sizeof(freq_list)        / sizeof(freq_list[0]); }
uint16_t radio_get_bandwidth_length() { return sizeof(bandwidth_list)   / sizeof(bandwidth_list[0]); }
uint16_t radio_get_tx_power_length()  { return sizeof(power_level_list) / sizeof(power_level_list[0]); }

float radio_get_freq_from_index(uint8_t index)
{
    if (index >= radio_get_freq_length()) return 433.0;
    return freq_list[index];
}

float radio_get_bandwidth_from_index(uint8_t index)
{
    if (index >= radio_get_bandwidth_length()) return 102.0;
    return bandwidth_list[index];
}

float radio_get_tx_power_from_index(uint8_t index)
{
    if (index >= radio_get_tx_power_length()) return 10;
    return power_level_list[index];
}

#endif  // ARDUINO_LILYGO_LORA_CC1101
