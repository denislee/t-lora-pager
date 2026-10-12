/**
 * @file      display.cpp
 * @brief     Display backlight, brightness, keyboard input.
 */
#include "display.h"
#include "system.h"
#include "internal.h"
#include "../core/kb_lock.h"

#ifdef ARDUINO
#include <LilyGoLib.h>
#endif

void hw_set_disp_backlight(uint8_t level)
{
#ifdef ARDUINO
    instance.setBrightness(level);
#endif
}

void hw_set_kb_backlight(uint8_t level)
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_KEYBOARD)
    core::ScopedKbLock lock;
    instance.kb.setBrightness(level);
#endif
}

uint8_t hw_get_disp_min_brightness()
{
    return dev_conts_var.min_brightness;
}

uint16_t hw_get_disp_max_brightness()
{
    return dev_conts_var.max_brightness;
}

void hw_enable_keyboard()
{
#if defined(ARDUINO)
    core::ScopedKbLock lock;
#if defined(ARDUINO_T_LORA_PAGER)
    instance.initKeyboard();
#endif
    // Ensure user setting is restored as initKeyboard might reset it to library defaults
    hw_set_kb_backlight(user_setting.keyboard_bl_level);
    // Disable the vendor driver's repeat emitter. With it on, backspace
    // fires KB_PRESSED every 300 ms while held, which is indistinguishable
    // from a fast re-tap — a user pressing backspace twice in under 320 ms
    // would look identical to "still held" and the second tap would be
    // swallowed. Off means one KB_PRESSED per physical tap: no auto-repeat
    // on hold (by design) and every fast re-tap registers.
#if defined(ARDUINO_T_LORA_PAGER)
    instance.kb.setRepeat(false);
#endif
#endif
}

void hw_disable_keyboard()
{
#if defined(ARDUINO)
#if defined(ARDUINO_T_LORA_PAGER)
    core::ScopedKbLock lock;
    instance.kb.end();
#endif
#endif
}

bool hw_has_keyboard()
{
    return hw_get_device_online() & HW_KEYBOARD_ONLINE;
}

void hw_set_keyboard_read_callback(void(*read)(int state, char &c))
{
#if defined(ARDUINO) && defined(USING_INPUT_DEV_KEYBOARD)
    core::ScopedKbLock lock;
    instance.kb.setCallback(read);
#endif
}
