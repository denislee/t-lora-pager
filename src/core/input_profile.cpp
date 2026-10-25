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
#include <esp_private/esp_clk.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <LilyGoLib.h>      // KB_PRESSED / KB_RELEASED
#include <lvgl_private.h>   // lv_timer_t fields for the slow-frame census
#include "../hal/keyboard_task.h"
#include "../hal/rotary_task.h"
#include "../hal/storage.h"
#include "../hal/notes_crypto.h"
#include "../hal/system.h"
#include "../hal/pbkdf2_sha256.h"
#include <string>
#include <vector>
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

// --- per-stage frame timing (P7.9) ----------------------------------------
// Refresh span comes from LVGL's REFR_START/REFR_READY events; swap and push
// come from the vendor disp_flush hook (lvh_prof_flush).
uint64_t s_refr_t0 = 0;
uint32_t s_fr_refr_us = 0;
uint32_t s_fr_swap_us = 0;
uint32_t s_fr_push_us = 0;
uint32_t s_fr_px = 0;
uint32_t s_fr_flushes = 0;

// Window counterparts, reset by report_tick().
uint32_t s_win_refr_max_us = 0;
uint64_t s_win_px = 0;
uint32_t s_win_refr_frames = 0;

bool s_refr_hooked = false;

// --- boot markers (G.1) ---
static const uint32_t kBootMax = 32;
const char *s_boot_name[kBootMax];
uint64_t s_boot_us[kBootMax];
uint32_t s_boot_n = 0;
bool s_boot_flushed = false;   // first display flush seen
bool s_boot_printed = false;
uint64_t s_pre_rtc_us = 0;      // esp_clk_rtc_time() at boot_pre_app()
uint64_t s_pre_timer_us = 0;    // esp_timer time at the same call
int s_pre_reason = -1;          // esp_reset_reason(), -1 = not recorded

void refr_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        s_refr_t0 = now_us();
    } else if (code == LV_EVENT_REFR_READY && s_refr_t0 != 0) {
        s_fr_refr_us += (uint32_t)(now_us() - s_refr_t0);
        s_refr_t0 = 0;
    }
}

// --- named spans -----------------------------------------------------------
const char *s_span_max_name = "?";
uint64_t s_span_max_us = 0;

// --- slow-frame timer census (device only) --------------------------------
#ifdef ARDUINO
static const uint32_t kCensusMax = 64;
lv_timer_t *s_census_t[kCensusMax];
uint32_t s_census_run[kCensusMax];
uint32_t s_census_n = 0;
#endif

// --- serial console requests, served on the LVGL thread -------------------
volatile bool s_req_group = false;
volatile bool s_req_report = false;
volatile bool s_req_dirty = false;  // console 'f': hw_set_filesystem_dirty(true)
char s_req_app[24] = {0};  // console 'a <App>': queueEnterApp on the LVGL thread

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
    (void)wait_us;  // kb_reader stopped taking the instance lock in IS.1
    s_take_us = now_us();
    s_holder = current_task_name();
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

void boot_mark(const char *name) {
    if (s_boot_n < kBootMax) {
        s_boot_name[s_boot_n] = name;
        s_boot_us[s_boot_n] = now_us();
        s_boot_n++;
    }
}

void boot_pre_app() {
#ifdef ARDUINO
    s_pre_rtc_us = esp_clk_rtc_time();
    s_pre_timer_us = now_us();
    s_pre_reason = (int)esp_reset_reason();
#endif
}

// One display flush (vendor disp_flush hook): pixels, byte-swap and SPI push
// time (P7.9).
static void note_flush(uint32_t px, uint32_t swap_us, uint32_t push_us) {
    if (!s_boot_flushed) { s_boot_flushed = true; boot_mark("first flush"); }
    s_fr_px += px;
    s_fr_swap_us += swap_us;
    s_fr_push_us += push_us;
    s_fr_flushes++;
}

void frame_begin() {
    // Hook the display's refresh events once, as soon as LVGL has a default
    // display, so the REFR span covers render + flush + flush-ready wait.
    if (!s_refr_hooked) {
        lv_display_t *disp = lv_display_get_default();
        if (disp) {
            lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_START, NULL);
            lv_display_add_event_cb(disp, refr_event_cb, LV_EVENT_REFR_READY, NULL);
            s_refr_hooked = true;
        }
    }
    s_fr_refr_us = 0;
    s_fr_swap_us = 0;
    s_fr_push_us = 0;
    s_fr_px = 0;
    s_fr_flushes = 0;
#ifdef ARDUINO
    s_census_n = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t && s_census_n < kCensusMax;
         t = lv_timer_get_next(t)) {
        s_census_t[s_census_n] = t;
        s_census_run[s_census_n] = t->last_run;
        s_census_n++;
    }
#endif
}

void frame(uint64_t us) {
#ifdef ARDUINO
    // Name the timers that ran in a slow frame: last_run moved, or the timer
    // is new. A timer deleted during the frame is not listed. Resolve cb with
    // addr2line -fe .pio/build/tlora_pager_profile/firmware.elf <cb>.
    if (us >= 100000) {
        // Split the refresh span into render / swap / push so a slow frame
        // points at the stage that actually cost the time (P7.9).
        int64_t refr_us   = (int64_t)s_fr_refr_us;
        int64_t swap_us   = (int64_t)s_fr_swap_us;
        int64_t push_us   = (int64_t)s_fr_push_us;
        int64_t render_us = refr_us - swap_us - push_us;
        if (render_us < 0) render_us = 0;
        int64_t other_us  = (int64_t)us - refr_us;
        if (other_us < 0) other_us = 0;
        PROF_LOG("slow frame %u ms @ %u MHz: refr %u ms (render %u, swap %u, push %u ms; %u px in %u flushes), timers/other %u ms; timers that ran:",
                 (unsigned)(us / 1000), (unsigned)getCpuFrequencyMhz(),
                 (unsigned)(refr_us / 1000), (unsigned)(render_us / 1000),
                 (unsigned)(swap_us / 1000), (unsigned)(push_us / 1000),
                 (unsigned)s_fr_px, (unsigned)s_fr_flushes,
                 (unsigned)(other_us / 1000));
        for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
            bool seen = false, ran = true;
            for (uint32_t i = 0; i < s_census_n; i++) {
                if (s_census_t[i] == t) { seen = true; ran = s_census_run[i] != t->last_run; break; }
            }
            if (ran) PROF_LOG("  cb=%p period=%u%s", (void *)t->timer_cb,
                              (unsigned)t->period, seen ? "" : " (new)");
        }
    }
#endif
    if (us > s_frame_max_us) s_frame_max_us = us;
    if (us > 33000) s_frame_gt33++;
    if (us > 100000) s_frame_gt100++;
    s_frame_count++;

    if (s_fr_refr_us > s_win_refr_max_us) s_win_refr_max_us = s_fr_refr_us;
    s_win_px += s_fr_px;
    if (s_fr_refr_us) s_win_refr_frames++;
}

void span(const char *name, uint64_t us) {
    if (us >= 5000) PROF_LOG("span %s %u ms", name, (unsigned)(us / 1000));
    if (us > s_span_max_us) {
        s_span_max_us = us;
        s_span_max_name = name;
    }
}

#ifdef ARDUINO
namespace {

void inject_char(char c) {
    hw_keyboard_inject(KB_PRESSED, c);
    vTaskDelay(pdMS_TO_TICKS(15));
    hw_keyboard_inject(KB_RELEASED, c);
    vTaskDelay(pdMS_TO_TICKS(25));
}

void run_command(char *line) {
    char *arg = line + 1;
    while (*arg == ' ') arg++;
    switch (line[0]) {
    case 'k':
        for (char *p = arg; *p; p++) {
            char c = *p;
            if (c == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'e': c = 27; break;
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'b': c = '\b'; break;
                default: c = *p; break;
                }
            }
            inject_char(c);
        }
        break;
    case 'r': {
        char d = *arg;
        int n = atoi(arg + 1);
        if (n <= 0) n = 1;
        for (int i = 0; i < n; i++) {
            hw_rotary_inject(d == 'u' ? 1 : d == 'd' ? -1 : 0, d == 'c');
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        break;
    }
    case 'b': {
        int n = atoi(arg);
        char *sp = strchr(arg, ' ');
        int ms = sp ? atoi(sp + 1) : 0;
        for (int i = 0; i < n; i++) {
            char c = 'a' + (i % 26);
            hw_keyboard_inject(KB_PRESSED, c);
            hw_keyboard_inject(KB_RELEASED, c);
            if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
        }
        break;
    }
    case 'g': s_req_group = true; break;
    case 'f': s_req_dirty = true; break;
    case 'p': {
        // Time one PBKDF2-HMAC-SHA256 x10000 / 48 B, the derive_key_iv() work
        // (hal::pbkdf2_sha256 since P7.2, not mbedTLS).
        const uint8_t salt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        uint8_t out[48];
        uint64_t t0 = now_us();
        hal::pbkdf2_hmac_sha256((const uint8_t *)"benchmark-pw", 12, salt, sizeof(salt),
                                10000, out, sizeof(out));
        uint64_t us = now_us() - t0;
        PROF_LOG("pbkdf2 x10000: %u ms at %u MHz (task prio %u)", (unsigned)(us / 1000),
                 (unsigned)getCpuFrequencyMhz(), (unsigned)uxTaskPriorityGet(NULL));
        break;
    }
    case 'j': {
        PROF_LOG("crypto enabled=%d unlocked=%d", (int)notes_crypto_is_enabled(),
                 (int)notes_crypto_is_unlocked());
        std::vector<std::pair<std::string, uint32_t>> files;
        uint64_t t0 = now_us();
        hw_get_preferred_txt_files_info(files, nullptr);
        PROF_LOG("scan: %u files in %u ms", (unsigned)files.size(),
                 (unsigned)((now_us() - t0) / 1000));
        break;
    }
    case 'a':
        strncpy(s_req_app, arg, sizeof(s_req_app) - 1);
        s_req_app[sizeof(s_req_app) - 1] = '\0';
        break;
    case 's': s_req_report = true; break;
    default: break;
    }
    PROF_LOG("cmd done: %c", line[0]);
}

void console_task(void *) {
    char line[256];
    size_t n = 0;
    for (;;) {
        while (Serial.available()) {
            int ch = Serial.read();
            if (ch < 0) break;
            if (ch == '\r') continue;
            if (ch == '\n') {
                line[n] = '\0';
                if (n > 0) run_command(line);
                n = 0;
            } else if (n < sizeof(line) - 1) {
                line[n++] = (char)ch;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

} // namespace
#endif

void report_tick() {
    uint64_t now = now_us();
    if (!s_report_started) {
        s_last_report_us = now;
        s_report_started = true;
#ifdef ARDUINO
        xTaskCreatePinnedToCore(console_task, "prof_console", 4096, NULL, 2,
                                NULL, 0);
#endif
        return;
    }
    if (s_boot_flushed && !s_boot_printed) {
        s_boot_printed = true;
        if (s_pre_reason >= 0) {
            // Pre-app = ROM + 2nd-stage bootloader + early IDF init, before esp_timer
            // started. Add it to every esp_timer mark below for power-on time.
            // Only meaningful after a power-on / chip reset (the RTC timer survives esp_restart).
            PROF_LOG("boot: reset reason %d, rtc %u ms at setup, pre-app %u ms", s_pre_reason,
                     (unsigned)(s_pre_rtc_us / 1000),
                     (unsigned)((s_pre_rtc_us > s_pre_timer_us ? s_pre_rtc_us - s_pre_timer_us : 0) / 1000));
        }
        // Times are esp_timer microseconds (esp_timer starts early in IDF startup, so
        // ROM/bootloader time before it is not included).
        uint64_t prev = 0;
        for (uint32_t i = 0; i < s_boot_n; i++) {
            PROF_LOG("boot: %-14s %6u ms (+%u ms)", s_boot_name[i],
                     (unsigned)(s_boot_us[i] / 1000), (unsigned)((s_boot_us[i] - prev) / 1000));
            prev = s_boot_us[i];
        }
    }
    if (s_req_app[0]) {
        PROF_LOG("console: enter app %s", s_req_app);
        // queueEnterApp keeps the pointer until System::loop applies it.
        static char s_app_name[24];
        memcpy(s_app_name, s_req_app, sizeof(s_app_name));
        s_req_app[0] = '\0';
        core::System::getInstance().queueEnterApp(s_app_name);
    }
    if (s_req_dirty) {
        s_req_dirty = false;
        hw_set_filesystem_dirty(true);
        PROF_LOG("console: filesystem marked dirty");
    }
    if (s_req_group) {
        s_req_group = false;
        dump_group("console");
    }
    bool forced = s_req_report;
    s_req_report = false;
    if (!forced && now - s_last_report_us < 10000000ULL) return;
    s_last_report_us = now;

    if (s_slow_pending) {
        PROF_LOG("slow hold %u ms by %s", (unsigned)(s_slow_us / 1000),
                 s_slow_name);
        s_slow_pending = false;
    }

#ifdef ARDUINO
    // Heap and stack telemetry (P7.49) print every window, even when nothing
    // else was recorded: a heap/stack problem is worth seeing while idle.
    heap_stats_t hs;
    hw_get_heap_stats(hs);
    PROF_LOG("heap: int free %uK min %uK blk %uK | psram free %uK min %uK",
             (unsigned)(hs.int_free / 1024), (unsigned)(hs.int_min_free / 1024),
             (unsigned)(hs.int_largest / 1024), (unsigned)(hs.psram_free / 1024),
             (unsigned)(hs.psram_min_free / 1024));

    // Stack high-water marks (minimum free since task start), P7.49. By name:
    // the 2.0.17 SDK has no trace facility, so no uxTaskGetSystemState.
    static const char *const kTasks[] = {
        "lvgl", "kb_reader", "rotary_reader", "rotary", "charge_ind",
        "nfc_reader", "link", "ble_kb_ka", "nimble_host", "loopTask",
        "ssh_app", "app/play", "app/rec", "note_save", "prof_console",
    };
    char buf[384];
    size_t off = 0;
    buf[0] = '\0';
    for (const char *name : kTasks) {
        TaskHandle_t h = xTaskGetHandle(name);
        if (!h) continue;
        int wrote = snprintf(buf + off, sizeof(buf) - off, "%s:%uB ", name,
                             (unsigned)(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t)));
        if (wrote < 0) break;
        off += (size_t)wrote;
        if (off >= sizeof(buf)) { off = sizeof(buf) - 1; break; }
    }
    PROF_LOG("stack free: %s", buf);
#endif

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
        "frame max %ums >33:%u >100:%u | span max %s %ums | "
        "refr max %ums, %u frames, avg %u px",
        (unsigned)(s_hold_max_us / 1000), s_hold_max_holder,
        (unsigned)s_hist[0], (unsigned)s_hist[1], (unsigned)s_hist[2],
        (unsigned)s_hist[3], (unsigned)s_hist[4], (unsigned)s_hist[5],
        (unsigned)(s_kb_wait_max_us / 1000),
        (unsigned)s_key_count, p50, p95, (unsigned)(s_key_max_us / 1000),
        (unsigned)(s_frame_max_us / 1000), (unsigned)s_frame_gt33,
        (unsigned)s_frame_gt100, s_span_max_name,
        (unsigned)(s_span_max_us / 1000),
        (unsigned)(s_win_refr_max_us / 1000), (unsigned)s_win_refr_frames,
        (unsigned)(s_win_refr_frames ? s_win_px / s_win_refr_frames : 0));

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
    s_win_refr_max_us = 0;
    s_win_px = 0;
    s_win_refr_frames = 0;
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

// P7.9: display flush hook, called from the vendor disp_flush() (LV_Helper_v9.cpp).
extern "C" void lvh_prof_flush(uint32_t px, uint32_t swap_us, uint32_t push_us) {
    core::prof::note_flush(px, swap_us, push_us);
}

// G.1: boot marker hook, called from the vendor LilyGoLoRaPager::begin().
extern "C" void lvh_prof_boot_mark(const char *name) {
    core::prof::boot_mark(name);
}

#endif // INPUT_PROFILE
