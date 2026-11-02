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

#include "hal_interface.h"
#include "core/system.h"
#include "apps/app_registry.h"
#include "emu_shots.h"
#include "hal/link.h"

// hw_init() is declared in hal/system.h (reachable via hal_interface.h).
// instanceLockTake/Give come from core/instance_lock.cpp — compiled as no-ops
// when !ARDUINO so this TU no longer needs to define them.

static lv_display_t *lvDisplay;

// KF.25b: the device's invalidate-area rounder (vendor lv_rounder_cb in lib/LilyGoLib/src/LV_Helper_v9.cpp, same rules), so the probe's px column matches what the device flushes. -D EMU_NO_DEVICE_ROUNDER turns it off.
#ifndef EMU_NO_DEVICE_ROUNDER
static void emu_rounder_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
#if defined(ARDUINO_T_WATCH_S3_ULTRA)
#if defined(LVH_ULTRA_X1_ALIGN) && LVH_ULTRA_X1_ALIGN > 1
    area->x1 &= ~(int32_t)(LVH_ULTRA_X1_ALIGN - 1);
#else
    if (area->x1 > 1) area->x1 = 0;
#endif
#endif
    if (!(area->x2 & 1)) area->x2++;
    if (area->y1 & 1) area->y1--;
    if (!(area->y2 & 1)) area->y2++;
}
#endif


void hal_setup(void)
{
    // Workaround for sdl2 `-m32` crash
    // https://bugs.launchpad.net/ubuntu/+source/libsdl2/+bug/1775067/comments/7
#ifndef WIN32
    setenv("DBUS_FATAL_WARNINGS", "0", 1);
#endif

    // The screenshot sweep renders the Journal, Tasks and editor from the
    // emulator's note storage (the PagerLink directories, fs_volume.h): give
    // it a fresh empty root so the shots do not depend on the host's notes.
    if (getenv("UI_SHOTS_DIR") && !getenv("PAGERLINK_FS_ROOT")) {
        char root[] = "/tmp/ui-shots-fs-XXXXXX";
        if (mkdtemp(root)) setenv("PAGERLINK_FS_ROOT", root, 1);
    }

    /* Add a display
     * Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/


    lvDisplay = lv_sdl_window_create(SDL_HOR_RES, SDL_VER_RES);
#ifndef EMU_NO_DEVICE_ROUNDER
    lv_display_add_event_cb(lvDisplay, emu_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#endif
    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    lv_sdl_keyboard_create();
}

void hal_loop(void)
{
    Uint32 lastTick = SDL_GetTicks();
    uint32_t wait = 16;
    while (1) {
        fflush(stdout);
        SDL_Delay(wait);
        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick);
        lastTick = current;
        // KF.25a: sleep until LVGL's next timer, at most 16 ms (a static screen still sleeps the full 16 ms), at least 1 ms.
        uint32_t next = lv_timer_handler();
        core::System::getInstance().loop();
        wait = next > 16 ? 16 : (next < 1 ? 1 : next);
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
    if (getenv("KITCHEN_BENCH")) return emu_kitchen_bench(lvDisplay);

    // PagerLink companion protocol over TCP (127.0.0.1:7600, PAGERLINK_PORT=0
    // disables) so the iOS app in the Simulator can talk to the emulator.
    hw_link_start();

    hal_loop();
}
#endif
