/**
 * @file      main.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-08
 *
 */

#ifndef ARDUINO
#include <stdio.h>
#include "lvgl.h"
#include <unistd.h>
#include <stdlib.h>
#define SDL_MAIN_HANDLED        /*To fix SDL's "undefined reference to WinMain" issue*/
#include <SDL2/SDL.h>
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"
#include "drivers/sdl/lv_sdl_keyboard.h"
#include "demos/lv_demos.h"
#include "examples/lv_examples.h"

#include "hal_interface.h"
#include "core/system.h"
#include "core/system_hooks.h"
#include "apps/app_registry.h"
#include "emu_shots.h"
#include "hal/link.h"

// hw_init() is declared in hal/system.h (reachable via hal_interface.h).
// instanceLockTake/Give come from core/instance_lock.cpp — compiled as no-ops
// when !ARDUINO so this TU no longer needs to define them.

static lv_display_t *lvDisplay;
static lv_indev_t *lvMouse;
static lv_indev_t *lvMouseWheel;
static lv_indev_t *lvKeyboard;


#if LV_USE_LOG != 0
static void lv_log_print_g_cb(lv_log_level_t level, const char * buf)
{
    LV_UNUSED(level);
    LV_UNUSED(buf);
}
#endif


void hal_setup(void)
{
    // Workaround for sdl2 `-m32` crash
    // https://bugs.launchpad.net/ubuntu/+source/libsdl2/+bug/1775067/comments/7
#ifndef WIN32
    setenv("DBUS_FATAL_WARNINGS", "0", 1);
#endif

#if LV_USE_LOG != 0
    lv_log_register_print_cb(lv_log_print_g_cb);
#endif

    /* Add a display
     * Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/


    lvDisplay = lv_sdl_window_create(SDL_HOR_RES, SDL_VER_RES);
    lvMouse = lv_sdl_mouse_create();
    lvMouseWheel = lv_sdl_mousewheel_create();
    lvKeyboard = lv_sdl_keyboard_create();
}

void hal_loop(void)
{
    Uint32 lastTick = SDL_GetTicks();
    while (1) {
        fflush(stdout);
        SDL_Delay(16);
        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick);
        lastTick = current;
        lv_timer_handler();
        core::System::getInstance().loop();
    }
}


extern "C" int main(void)
{
    lv_init();

    hal_setup();
    printf("hello lvgl\n");
    // hw_init() no longer loads settings itself (factory.ino loads them
    // before hw_init() on hardware); populate the emulator defaults here.
    hw_load_setting();
    hw_init();
    // No-op on the emulator (connectivity toggles are Arduino-only), kept for
    // parity with the hardware setup() sequence.
    hw_connectivity_init();

    apps::register_all();
    core::System::getInstance().init();

    // UI_SHOTS_DIR=<dir>: render every page in every style / dark-mode
    // combination to <dir> and exit instead of running interactively.
    if (const char *shots = getenv("UI_SHOTS_DIR")) {
        return emu_shots_run(lvDisplay, shots);
    }

    // PagerLink companion protocol over TCP (127.0.0.1:7600, PAGERLINK_PORT=0
    // disables) so the iOS app in the Simulator can talk to the emulator.
    hw_link_enable(true);

    hal_loop();
}
#endif
