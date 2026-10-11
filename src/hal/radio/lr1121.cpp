/**
 * @file      hw_lr1121.cpp
 * @brief     LR1121 (multi-band LoRa incl. 2.4 GHz) — per-chip programming.
 *
 * Shared ISR/event-group/TX/RX plumbing lives in hal/radio_common.cpp.
 * LR1121 is unique among the supported modules in that it spans sub-GHz and
 * 2.4 GHz; the option tables and output-power caps switch based on
 * `_high_freq`, which tracks the band of the last selected frequency.
 */

#include "../../hal_interface.h"
#include "../radio_chip.h"

#ifdef ARDUINO_LILYGO_LORA_LR1121

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

static bool _high_freq = false;

namespace radio_chip {

void default_params(radio_params_t &params)
{
    params.bandwidth = 125.0;
    params.freq      = RADIO_DEFAULT_FREQUENCY;
    params.cr        = 5;
    params.mode      = RADIO_DISABLE;
    params.sf        = 12;
    params.power     = 22;
    params.syncWord  = 0xCD;
}

int16_t configure(const radio_params_t &params)
{
    _high_freq = (params.freq > 960.0);

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

    switch (params.mode) {
    case RADIO_DISABLE: state = radio.standby();         break;
    case RADIO_TX:      state = radio.startTransmit(""); break;
    case RADIO_RX:      state = radio.startReceive();    break;
    case RADIO_CW:
        radio.standby();
        delay(5);
        radio.transmitDirect();
        break;
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

#ifdef RADIO_FIXED_FREQUENCY
static const float freq_list[] = {
    RADIO_FIXED_FREQUENCY,
    2400.0, 2410.0, 2420.0, 2430.0, 2440.0, 2450.0, 2460.0, 2470.0, 2480.0, 2490.0, 2500.0
};
#else
static const float freq_list[] = {
    315.0, 433.0, 434.0, 470.0, 842.0, 850, 868.0, 915.0, 923.0, 945.0,
    2400.0, 2410.0, 2420.0, 2430.0, 2440.0, 2450.0, 2460.0, 2470.0, 2480.0, 2490.0, 2500.0
};
#endif

static const float bandwidth_list[]           = {62.5, 125.0, 250.0, 500.0};
static const float bandwidth_high_freq_list[] = {62.5, 125.0, 203.125, 250.0, 406.25, 500.0, 812.5};

static const float power_level_list[]           = {2, 5, 10, 12, 17, 20, 22};
static const float power_level_high_freq_list[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

uint16_t radio_get_freq_length()
{
    return sizeof(freq_list) / sizeof(freq_list[0]);
}

uint16_t radio_get_bandwidth_length()
{
    if (_high_freq) return sizeof(bandwidth_high_freq_list) / sizeof(bandwidth_high_freq_list[0]);
    return sizeof(bandwidth_list) / sizeof(bandwidth_list[0]);
}

uint16_t radio_get_tx_power_length()
{
    if (_high_freq) return sizeof(power_level_high_freq_list) / sizeof(power_level_high_freq_list[0]);
    return sizeof(power_level_list) / sizeof(power_level_list[0]);
}

float radio_get_freq_from_index(uint8_t index)
{
    if (index >= radio_get_freq_length()) {
        _high_freq = false;
        return RADIO_DEFAULT_FREQUENCY;
    }
    return freq_list[index];
}

float radio_get_bandwidth_from_index(uint8_t index)
{
    if (_high_freq) {
        if (index >= sizeof(bandwidth_high_freq_list) / sizeof(bandwidth_high_freq_list[0])) index = 0;
        return bandwidth_high_freq_list[index];
    }
    if (index >= sizeof(bandwidth_list) / sizeof(bandwidth_list[0])) index = 0;
    return bandwidth_list[index];
}

float radio_get_tx_power_from_index(uint8_t index)
{
    if (_high_freq) {
        if (index >= sizeof(power_level_high_freq_list) / sizeof(power_level_high_freq_list[0])) return 13.0;
        return power_level_high_freq_list[index];
    }
    if (index >= sizeof(power_level_list) / sizeof(power_level_list[0])) return 22.0;
    return power_level_list[index];
}

#endif  // ARDUINO_LILYGO_LORA_LR1121
