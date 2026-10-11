/**
 * @file      board_config.h
 * @brief     Per-board compile-time configuration (capabilities, fonts, tips).
 *
 * Activated by the ARDUINO_T_* defines set by PlatformIO board variants.
 */
#pragma once

#if defined(ARDUINO_T_LORA_PAGER)

#define USING_BLE_KEYBOARD
#define FLOAT_BUTTON_WIDTH   40
#define FLOAT_BUTTON_HEIGHT  40

// LibSSH-ESP32 is only in the tlora_pager lib_deps; other boards fall back
// to the SSH app's loopback backend.
#define USING_LIBSSH

#ifndef RADIOLIB_EXCLUDE_NRF24
#define USING_EXTERN_NRF2401
#endif

// The Pager carries a BHI260AP (also used as the GPIO expander). Without
// this define every #ifdef USING_BHI260_SENSOR block in src/hal/sensors.cpp
// and src/hal/system.cpp compiles away — which is why the IMU debug page
// reports all zeros.
#ifndef USING_BHI260_SENSOR
#define USING_BHI260_SENSOR
#endif
/* USING_ST25R3916 is injected as a -D build flag in platformio.ini so it
 * reaches every translation unit (incl. hal/nfc_reader.cpp which doesn't
 * pull in this header). */

#define MAIN_FONT   &lv_font_montserrat_20

#elif defined(ARDUINO_T_WATCH_S3_ULTRA)

#define USING_TOUCHPAD
#define FLOAT_BUTTON_WIDTH   60
#define FLOAT_BUTTON_HEIGHT  60
#define USING_BLE_KEYBOARD

#ifndef USING_BHI260_SENSOR
#define USING_BHI260_SENSOR
#endif
#ifndef USING_ST25R3916
#define USING_ST25R3916
#endif
#ifndef HAS_USB_RF_SWITCH
#define HAS_USB_RF_SWITCH
#endif

#define MAIN_FONT   &lv_font_montserrat_22

#elif defined(ARDUINO_T_WATCH_S3)

#define USING_TOUCHPAD
#define FLOAT_BUTTON_WIDTH   40
#define FLOAT_BUTTON_HEIGHT  40

#ifndef USING_BMA423_SENSOR
#define USING_BMA423_SENSOR
#define USING_BLE_KEYBOARD
#endif

#define MAIN_FONT   &lv_font_montserrat_16

#endif
