/**
 * @file      nrf2401.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-04-24
 *
 */

#include "../../hal_interface.h"
#include "../../core/spi_lock.h"

#if defined(USING_EXTERN_NRF2401)

#ifdef ARDUINO

#include <LilyGoLib.h>

// Probe only (sets HW_NRF24_ONLINE for the device-probe page); nothing
// transmits or receives on the nRF24 (DC2.3.13).
void hw_nrf24_begin()
{
    log_d("init NRF2401");
    bool rlst = instance.initNRF24();
    if (!rlst) {
        log_e("nRF2401 option Model not detected");
        return;
    }

    // Set PA control IO to output function
    instance.io.pinMode(EXPANDS_GPIO_EN, OUTPUT);
}
#endif

#endif
