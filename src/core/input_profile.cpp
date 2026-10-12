/**
 * @file      input_profile.cpp
 * @brief     Input-stutter profiling (plan item IS.0) — see input_profile.h.
 *
 * Compiled in only when `INPUT_PROFILE` is defined (`pio run -e
 * tlora_pager_profile`). All recording functions run with the instance
 * lock held or on the single LVGL thread, so they add no locking of their
 * own and are kept cheap; the only output is the periodic `[prof]`
 * summary and any slow span / slow hold line.
 */
#include "input_profile.h"

#ifdef INPUT_PROFILE

#include "system.h"
#include <lvgl.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <chrono>
#endif

// Do NOT use log_i: CORE_DEBUG_LEVEL=0 compiles it out.
#ifdef ARDUINO
#define PROF_LOG(fmt, ...) Serial.printf("[prof] " fmt "\n", ##__VA_ARGS__)
#else
#define PROF_LOG(fmt, ...) printf("[prof] " fmt "\n", ##__VA_ARGS__)
#endif

namespace core { namespace prof {

uint64_t now_us() {
#ifdef ARDUINO
    return (uint64_t)esp_timer_get_time();
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
#endif
}

namespace {

const char *current_task_name() {
#ifdef ARDUINO
    return pcTaskGetName(NULL);
#else
    return "main";
#endif
}

// --- instance-lock tracker -------------------------------------------------
uint64_t s_take_us = 0;
const char *s_holder = "?";

uint64_t s_hold_max_us = 0;
const char *s_hold_max_holder = "?";
uint32_t s_hist[6] = {0, 0, 0, 0, 0, 0}; // <5 <16 <33 <100 <300 >=300 (ms)
bool s_lock_seen = false;

// Pending slow-hold slot, filled while the lock is held and printed by
// report_tick (never printed from lock_released).
char s_slow_name[16] = {0};
uint64_t s_slow_us = 0;
bool s_slow_pending = false;

// --- keyboard reader lock wait --------------------------------------------
uint64_t s_kb_wait_max_us = 0;

// --- key enqueue->dequeue latency -----------------------------------------
static const uint32_t kKeyRingSize = 128;
uint32_t s_key_ring[kKeyRingSize];
uint32_t s_key_head = 0;  // next write slot (overwrites oldest when full)
uint32_t s_key_count = 0; // live entries in the window
uint64_t s_key_max_us = 0;

// --- frame timing ----------------------------------------------------------
uint64_t s_frame_max_us = 0;
uint32_t s_frame_gt33 = 0;
uint32_t s_frame_gt100 = 0;
uint32_t s_frame_count = 0;

// --- named spans -----------------------------------------------------------
const char *s_span_max_name = "?";
uint64_t s_span_max_us = 0;

// --- report cadence --------------------------------------------------------
uint64_t s_last_report_us = 0;
bool s_report_started = false;

void hist_add(uint64_t hold_us) {
    uint64_t ms = hold_us / 1000;
    if (ms < 5) s_hist[0]++;
    else if (ms < 16) s_hist[1]++;
    else if (ms < 33) s_hist[2]++;
    else if (ms < 100) s_hist[3]++;
    else if (ms < 300) s_hist[4]++;
    else s_hist[5]++;
}

} // namespace

void lock_acquired(uint64_t wait_us) {
    s_take_us = now_us();
    s_holder = current_task_name();
    if (strcmp(s_holder, "kb_reader") == 0 && wait_us > s_kb_wait_max_us) {
        s_kb_wait_max_us = wait_us;
    }
}

void lock_released() {
    uint64_t hold = now_us() - s_take_us;

    if (hold > s_hold_max_us) {
        s_hold_max_us = hold;
        s_hold_max_holder = s_holder;
    }
    hist_add(hold);
    s_lock_seen = true;

    if (hold >= 50000) {
        const char *who = s_holder ? s_holder : "?";
        strncpy(s_slow_name, who, sizeof(s_slow_name) - 1);
        s_slow_name[sizeof(s_slow_name) - 1] = '\0';
        s_slow_us = hold;
        s_slow_pending = true;
    }
}

void kb_lock_wait(uint64_t wait_us) {
    if (wait_us > s_kb_wait_max_us) s_kb_wait_max_us = wait_us;
}

void key_latency(uint64_t us) {
    s_key_ring[s_key_head] = (uint32_t)us;
    s_key_head = (s_key_head + 1) % kKeyRingSize;
    if (s_key_count < kKeyRingSize) s_key_count++;
    if (us > s_key_max_us) s_key_max_us = us;
}

void frame(uint64_t us) {
    if (us > s_frame_max_us) s_frame_max_us = us;
    if (us > 33000) s_frame_gt33++;
    if (us > 100000) s_frame_gt100++;
    s_frame_count++;
}

void span(const char *name, uint64_t us) {
    if (us >= 5000) PROF_LOG("span %s %u ms", name, (unsigned)(us / 1000));
    if (us > s_span_max_us) {
        s_span_max_us = us;
        s_span_max_name = name;
    }
}

void report_tick() {
    uint64_t now = now_us();
    if (!s_report_started) {
        s_last_report_us = now;
        s_report_started = true;
        return;
    }
    if (now - s_last_report_us < 10000000ULL) return;
    s_last_report_us = now;

    if (s_slow_pending) {
        PROF_LOG("slow hold %u ms by %s", (unsigned)(s_slow_us / 1000),
                 s_slow_name);
        s_slow_pending = false;
    }

    bool any = s_lock_seen || s_key_count > 0 || s_frame_count > 0;
    if (!any) return;

    unsigned p50 = 0, p95 = 0;
    if (s_key_count > 0) {
        uint32_t tmp[kKeyRingSize];
        for (uint32_t i = 0; i < s_key_count; i++) tmp[i] = s_key_ring[i];
        std::sort(tmp, tmp + s_key_count);
        uint32_t i50 = (s_key_count * 50) / 100;
        uint32_t i95 = (s_key_count * 95) / 100;
        if (i50 >= s_key_count) i50 = s_key_count - 1;
        if (i95 >= s_key_count) i95 = s_key_count - 1;
        p50 = (unsigned)(tmp[i50] / 1000);
        p95 = (unsigned)(tmp[i95] / 1000);
    }

    PROF_LOG(
        "win: hold max %ums (%s) hist <5:%u <16:%u <33:%u <100:%u <300:%u "
        ">=300:%u | kb wait max %ums | key n=%u p50 %ums p95 %ums max %ums | "
        "frame max %ums >33:%u >100:%u | span max %s %ums",
        (unsigned)(s_hold_max_us / 1000), s_hold_max_holder,
        (unsigned)s_hist[0], (unsigned)s_hist[1], (unsigned)s_hist[2],
        (unsigned)s_hist[3], (unsigned)s_hist[4], (unsigned)s_hist[5],
        (unsigned)(s_kb_wait_max_us / 1000),
        (unsigned)s_key_count, p50, p95, (unsigned)(s_key_max_us / 1000),
        (unsigned)(s_frame_max_us / 1000), (unsigned)s_frame_gt33,
        (unsigned)s_frame_gt100, s_span_max_name,
        (unsigned)(s_span_max_us / 1000));

    // Reset all window stats.
    s_hold_max_us = 0;
    s_hold_max_holder = "?";
    for (int i = 0; i < 6; i++) s_hist[i] = 0;
    s_lock_seen = false;
    s_kb_wait_max_us = 0;
    s_key_head = 0;
    s_key_count = 0;
    s_key_max_us = 0;
    s_frame_max_us = 0;
    s_frame_gt33 = 0;
    s_frame_gt100 = 0;
    s_frame_count = 0;
    s_span_max_name = "?";
    s_span_max_us = 0;
}

void dump_group(const char *why) {
    lv_group_t *g = lv_group_get_default();
    if (!g) {
        PROF_LOG("group %s: none", why);
        return;
    }

    uint32_t n = lv_group_get_obj_count(g);
    PROF_LOG("group %s: %u members", why, (unsigned)n);

    lv_obj_t *menu = core::System::getInstance().getMenuPanel();
    lv_obj_t *app = core::System::getInstance().getAppPanel();
    lv_obj_t *back = core::System::getInstance().getBackButton();
    lv_obj_t *top = lv_layer_top();

    uint32_t lim = n < 64 ? n : 64;
    for (uint32_t i = 0; i < lim; i++) {
        lv_obj_t *obj = lv_group_get_obj_by_index(g, i);
        if (!obj) continue;

        const char *owner = "other";
        for (lv_obj_t *p = obj; p; p = lv_obj_get_parent(p)) {
            if (p == menu) { owner = "menu"; break; }
            if (p == app) { owner = "app"; break; }
            if (p == top) { owner = "top"; break; }
        }
        if (obj == back) owner = "back";

        const lv_obj_class_t *cls = lv_obj_get_class(obj);
        const char *cname = (cls && cls->name) ? cls->name : "?";

        PROF_LOG("  [%u] %s vis=%d owner=%s", (unsigned)i, cname,
                 (int)lv_obj_is_visible(obj), owner);
    }
    if (n > 64) PROF_LOG("  ... %u more", (unsigned)(n - 64));
}

}} // namespace core::prof

#endif // INPUT_PROFILE
