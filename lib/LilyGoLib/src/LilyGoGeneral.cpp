/**
 * @file      LilyGoGeneral.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2024  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2024-07-11
 *
 */
#include <Arduino.h>
#include "soc/rtc.h"
#include "esp_private/esp_clk.h"

// [LOCAL PATCH P6.1 / BT.4] One short start-up probe plus one calibration,
// instead of 6 x 5 x rtc_clk_cal(1000) (~0.80 s of busy-wait of which only one
// value was ever used). Mirrors ESP-IDF's own select_rtc_slow_clk(): switch the
// slow clock, calibrate RTC_MUX once (1024 cycles, ~31 ms) and publish the
// period with esp_clk_slowclk_cal_set() so RTC-timer timekeeping and deep-sleep
// timer wake-ups use the 32 kHz period (the vendor code never set it). The
// 100-cycle probe takes ~3 ms when the crystal runs and ~10 ms to time out when
// it does not; it is retried for up to 200 ms, which keeps the vendor's ~160 ms
// crystal start-up tolerance.
void esp_enable_slow_crystal()
{
    rtc_clk_32k_enable(true);

    uint32_t cal_32k = 0;
    const uint32_t t0 = millis();
    do {
        cal_32k = rtc_clk_cal(RTC_CAL_32K_XTAL, 100);
    } while (cal_32k == 0 && (millis() - t0) < 200);

    if (cal_32k == 0) {
        log_w("32K XTAL OSC has not started up, staying on the internal RC\n");
        return;
    }

    rtc_clk_slow_freq_set(RTC_SLOW_FREQ_32K_XTAL);
    uint32_t cal_slow = rtc_clk_cal(RTC_CAL_RTC_MUX, 1024);
    if (cal_slow != 0) {
        esp_clk_slowclk_cal_set(cal_slow);
    }

    if (rtc_clk_slow_freq_get() != RTC_SLOW_FREQ_32K_XTAL) {
        log_w("Failed to switch 32K XTAL RTC source to 32.768Khz !!! \n");
        return;
    }
    log_d("Switching RTC Source to 32.768Khz succeeded, using 32K XTAL\n");
}

void setGroupBitsFromISR(EventGroupHandle_t xEventGroup,
                         const EventBits_t uxBitsToSet)
{
    BaseType_t xHigherPriorityTaskWoken, xResult;
    xHigherPriorityTaskWoken = pdFALSE;
    xResult = xEventGroupSetBitsFromISR(xEventGroup, uxBitsToSet, &xHigherPriorityTaskWoken);
    if ( xResult == pdPASS ) {
        portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
    }
}
