/**
 * @file      radio.cpp
 * @brief     Radio enable/disable (LoRa/FSK via the radio_chip:: driver).
 *
 * The per-chip drivers live in hal/radio/<chip>.cpp. Nothing transmits or
 * receives (P4.18): enabling and disabling both program the startup settings
 * and park the chip, exactly as before the TX/RX/CW plumbing was removed
 * (DC2.3.13). The persisted radio_enable flag is the user-visible state.
 */
#include "radio.h"
#include "radio_chip.h"
#include "internal.h"
#include "../core/spi_lock.h"

bool hw_get_radio_enable() { return user_setting.radio_enable; }

int16_t hw_set_radio_enable(bool en)
{
    user_setting.radio_enable = en;
#ifdef ARDUINO
    radio_params_t params;
    radio_chip::default_params(params);
    core::ScopedSpiLock lock;
    return radio_chip::configure(params);
#else
    return 0;
#endif
}
