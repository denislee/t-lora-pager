/**
 * @file      radio_common.cpp
 * @brief     Shared radio config + boot ISR wiring for every LoRa/FSK RadioLib driver.
 *
 * Per-chip programming (setFrequency, setBandwidth, mode entry, ...) lives in
 * src/hw_<chip>.cpp and is reached through the `radio_chip::` hooks in
 * radio_chip.h. Exactly one per-chip driver is selected at build time via
 * ARDUINO_LILYGO_LORA_<MODULE>; this file is compiled unconditionally.
 */

#include "radio.h"
#include "radio_chip.h"
#include "../core/spi_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>

static EventGroupHandle_t radioEvent = NULL;

#define LORA_ISR_FLAG _BV(0)

static void hw_radio_isr()
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t xResult = xEventGroupSetBitsFromISR(
                             radioEvent, LORA_ISR_FLAG, &xHigherPriorityTaskWoken);
    if (xResult == pdPASS) {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void hw_radio_begin()
{
    radioEvent = xEventGroupCreate();
    radio.setPacketSentAction(hw_radio_isr);
}
#else
void hw_radio_begin() {}
#endif  // ARDUINO

int16_t hw_set_radio_params(radio_params_t &params)
{
#ifdef ARDUINO
    core::ScopedSpiLock lock;
    return radio_chip::configure(params);
#else
    (void)params;
    return 0;
#endif
}

void hw_get_radio_params(radio_params_t &params)
{
    radio_chip::default_params(params);
}

int16_t hw_set_radio_default()
{
    radio_params_t params;
    hw_get_radio_params(params);
    return hw_set_radio_params(params);
}
