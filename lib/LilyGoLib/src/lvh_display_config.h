/**
 * [LOCAL PATCH] Display-path switches (KITCHEN_FPS_PLAN_PART3 KF.26-29). All off by default, so a
 * plain build behaves as the vendor code does; the *_fps envs in platformio.ini turn them on.
 *
 * LVH_ASYNC_FLUSH=1     LVGL draws the next area while this one is being sent (KF.26).
 *                       Pager and Ultra: a flush task on core 0 swaps and pushes (KF.28).
 *                       Watch-S3: esp_lcd DMA, finished by on_color_trans_done.
 * LVH_DRAW_BUF_ROWS=n   Two internal-RAM draw bands of n rows of the screen's long side instead of two
 *                       full-screen PSRAM buffers; falls back to PSRAM if they don't fit (KF.27).
 * LVH_ULTRA_X1_ALIGN=n  Ultra only: align a dirty rect's x1 down to n (a power of two) instead of
 *                       moving it to 0 (KF.29). 0 keeps the vendor rule.
 */
#pragma once
#ifndef LVH_ASYNC_FLUSH
#define LVH_ASYNC_FLUSH 0
#endif
#ifndef LVH_DRAW_BUF_ROWS
#define LVH_DRAW_BUF_ROWS 0
#endif
#ifndef LVH_ULTRA_X1_ALIGN
#define LVH_ULTRA_X1_ALIGN 0
#endif
// Watch-S3's sync path reports the flush done while the DMA may still read the buffer. That is only
// safe because IDF 4.4 copies a PSRAM tx buffer at queue time; an internal buffer is read in place.
#if LVH_DRAW_BUF_ROWS > 0 && !LVH_ASYNC_FLUSH && defined(ARDUINO_T_WATCH_S3)
#error "LVH_DRAW_BUF_ROWS on the Watch-S3 needs LVH_ASYNC_FLUSH=1"
#endif
