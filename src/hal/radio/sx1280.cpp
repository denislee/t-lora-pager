/**
 * @file      hw_sx1280.cpp
 * @brief     SX1280 (2.4 GHz LoRa) — per-chip programming.
 *
 * Shared ISR/event-group/TX/RX plumbing lives in hal/radio_common.cpp.
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
    params.mode      = RADIO_DISABLE;
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

static const float bandwidth_list[]   = {203.125, 406.25, 812.5, 1625.0};
static const float power_level_list[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
static const float freq_list[]        = {
    2400.0, 2412.0, 2422.0, 2432.0, 2442.0, 2452.0,
    2462.0, 2472.0, 2482.0, 2492.0, 2500.0
};

uint16_t radio_get_freq_length()      { return sizeof(freq_list)        / sizeof(freq_list[0]); }
uint16_t radio_get_bandwidth_length() { return sizeof(bandwidth_list)   / sizeof(bandwidth_list[0]); }
uint16_t radio_get_tx_power_length()  { return sizeof(power_level_list) / sizeof(power_level_list[0]); }

float radio_get_freq_from_index(uint8_t index)
{
    if (index >= radio_get_freq_length()) return 2400.0;
    return freq_list[index];
}

float radio_get_bandwidth_from_index(uint8_t index)
{
    if (index >= radio_get_bandwidth_length()) return 203.125;
    return bandwidth_list[index];
}

float radio_get_tx_power_from_index(uint8_t index)
{
    if (index >= radio_get_tx_power_length()) return 13;
    return power_level_list[index];
}

#endif  // ARDUINO_LILYGO_LORA_SX1280
