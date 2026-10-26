/**
 * @file      ui_audio_notes.cpp
 * @brief     Audio "mental notes" app — record voice memos to SD, play them
 *            back, delete. All files live in /mental_notes/ on the SD card.
 *
 * Two entry points share this file: "Recorder" (home shortcut, quick mode)
 * starts a recording at once and exits when it stops; "Recordings"
 * (Settings) opens the list of notes, with playback one level below it.
 *
 * State reset on onStop (ui_notes_exit):
 *   - lv_obj_t*   menu, quit_btn   — destroyed via lv_obj_del / del_async
 *   - lv_obj_t*   main_page        — nulled (was child of menu, destroyed above)
 *   - lv_timer_t* tick_timer       — killed via kill_tick_timer()
 *   - s_notes_scan_job (SD scan worker, core::OneShotJob)
 *                                    — teardown(): a running worker is left
 *                                      to finish (it touches only a
 *                                      heap-local vector) and its result is
 *                                      freed on arrival unless a re-entry
 *                                      re-attaches it
 *   - s_notes_scan_pending           — cleared
 *   - active recording             — flushed via finalize_recording()
 *   - active playback              — stopped via hw_set_play_stop()
 *   - notes / selected_* / pending_rec_path — cleared
 *   - clear_view_refs()            — nulls per-view widget caches
 * If you add a new cached LVGL pointer, timer handle, or hardware session,
 * add it here AND extend ui_notes_exit() — leaks survive across app
 * switches and surface as ghost widgets / dead callback ptrs.
 */
#include "../ui_define.h"
#include "app_registry.h"
#include "../core/one_shot_job.h"
#include "../core/spi_lock.h"
#include "../hal/system.h"
#include "../hal/storage.h"
#include "../hal/audio.h"
#include <vector>
#include <string>
#include <algorithm>

#ifdef ARDUINO
#include <SD.h>
#include <LilyGoLib.h>
#endif

LV_FONT_DECLARE(font_montserrat_48_subset);  // P7.51 src/fonts/font_montserrat_48_subset.c

namespace {

static const char *NOTES_DIR = "/mental_notes";

enum ViewMode {
    VIEW_LIST,
    VIEW_RECORD,
    VIEW_PLAYBACK,
};

struct Note {
    std::string filename;   // leaf, e.g. "note_20260418_120530.wav"
    std::string full_path;  // "/mental_notes/note_20260418_120530.wav"
    uint32_t    mtime;
    uint32_t    data_bytes; // file size minus 44-byte header (for duration)
};

static lv_obj_t *menu = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_timer_t *tick_timer = NULL;

static ViewMode current_view = VIEW_LIST;
static std::vector<Note> notes;
static std::string selected_path;   // for playback view
static std::string selected_name;
static uint32_t    selected_bytes = 0;
static uint32_t    selected_mtime = 0;
static std::string pending_rec_path; // set while recording
// Quick-record mode: home-screen shortcut launches us straight into a
// recording. Stop / back / auto-cap exits the app. VIEW_RECORD is only ever
// entered in this mode.
static bool        s_quick_record = false;

static void ui_notes_enter(lv_obj_t *parent);
static void ui_notes_exit();
static void render_view();
static void enter_list();
static void enter_playback(const Note &n);

// --- helpers -----------------------------------------------------------

static std::string format_duration_s(uint32_t seconds)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u:%02u",
             (unsigned)(seconds / 60), (unsigned)(seconds % 60));
    return buf;
}

static std::string format_date(uint32_t t)
{
    if (t == 0) return "-";
    time_t tt = (time_t)t;
    struct tm info;
#ifdef ARDUINO
    localtime_r(&tt, &info);
#else
    struct tm *p = localtime(&tt);
    if (!p) return "-";
    info = *p;
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday);
    return buf;
}

static uint32_t bytes_to_seconds(uint32_t data_bytes)
{
    if (HW_REC_BYTES_PER_SEC == 0) return 0;
    return data_bytes / HW_REC_BYTES_PER_SEC;
}

static void ensure_notes_dir()
{
#ifdef ARDUINO
    if (!hw_sd_online()) return;
    core::ScopedSpiLock lock;
    if (!SD.exists(NOTES_DIR)) SD.mkdir(NOTES_DIR);
#endif
}

static std::string build_new_note_path()
{
    struct tm info = {};
    hw_get_date_time(info);
    char buf[64];
    snprintf(buf, sizeof(buf),
             "%s/note_%04d%02d%02d_%02d%02d%02d.wav",
             NOTES_DIR,
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday,
             info.tm_hour, info.tm_min, info.tm_sec);
    return buf;
}

// Pure scan: lists NOTES_DIR and fills `out`. Touches only its argument and
// the SD card (via ScopedSpiLock inside hw_list_sd_entries) — safe to run
// off the LVGL thread. Kept separate from `notes` so the background task
// below can build a result without racing the UI thread's reads of it.
static void scan_notes(std::vector<Note> &out)
{
    out.clear();
    std::vector<HwDirEntry> raw;
    hw_list_sd_entries(raw, ".wav", NOTES_DIR);
    for (const auto &r : raw) {
        if (r.is_dir) continue;
        Note n;
        n.filename  = r.path;
        n.full_path = std::string(NOTES_DIR) + "/" + r.path;
        n.mtime     = r.mtime;
        // WAV payload size = file size minus the 44-byte header. hw_list_sd_entries
        // already filled r.size from the directory scan, so there's no need to
        // re-open each file over the shared SPI bus just to read its length.
        n.data_bytes = (r.size >= 44) ? (r.size - 44) : 0;
        out.push_back(n);
    }
    // Sort newest first by mtime, tie-break on filename desc.
    std::sort(out.begin(), out.end(), [](const Note &a, const Note &b) {
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.filename > b.filename;
    });
}

static bool                s_notes_scan_pending = false;  // gates the list view's "Loading..." row

// Job delivery (LVGL thread): swap the freshly scanned list in and, if the
// list view is still on screen, re-render it.
static void notes_scan_done(std::vector<Note> &result)
{
    notes.swap(result);
    s_notes_scan_pending = false;
    if (current_view == VIEW_LIST) render_view();
}

// Background SD walk off the LVGL thread. The worker fills a heap-local
// vector, never the `notes` global or any widget.
static core::OneShotJob<std::vector<Note>> s_notes_scan_job(notes_scan_done);

// Kick a rescan of the notes directory off the LVGL thread. If a scan is
// already in flight (Attached: the app was left and re-entered mid-scan) the
// job re-attaches, so its result still lands. Either way the list shows
// "Loading..." until it does (DC2.0.7d).
static void kick_notes_reload()
{
    using Job = core::OneShotJob<std::vector<Note>>;
    // 4 KB stack: a plain single-directory SD walk, no TLS/HTTP involved.
    Job::Start started = s_notes_scan_job.start([]() {
        auto *result = new std::vector<Note>();
        scan_notes(*result);
        return result;
    }, 4096, "notes_scan", 1);
    s_notes_scan_pending = started != Job::Start::Failed;
    if (started == Job::Start::Failed) {
        scan_notes(notes); // fall back to a synchronous scan so the list isn't stuck empty
    }
}

// Switch to VIEW_LIST and (re)populate `notes`: start the background SD scan,
// then paint the list with whatever `notes` already holds (or "Loading...");
// notes_scan_done re-renders once the scan completes.
static void show_list_view()
{
    current_view = VIEW_LIST;
    kick_notes_reload();
    render_view();
}

// --- list view ---------------------------------------------------------

static void note_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= (int)notes.size()) return;
    enter_playback(notes[idx]);
}

static void build_list_view()
{
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_page, 6, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    // Notes list.
    lv_obj_t *list = lv_list_create(main_page);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_radius(list, UI_RADIUS, 0);

    if (notes.empty()) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, s_notes_scan_pending ? "Loading..." : "No notes yet");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
    } else {
        for (size_t i = 0; i < notes.size(); ++i) {
            const Note &n = notes[i];
            // Trailing: duration.
            std::string txt = format_duration_s(bytes_to_seconds(n.data_bytes));
            lv_obj_t *btn = ui_list_add_meta_row(list, LV_SYMBOL_AUDIO, n.filename.c_str(),
                                                 txt.c_str(), get_small_font());
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, note_click_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), btn);
        }
    }

    if (!notes.empty()) {
        lv_group_focus_obj(lv_obj_get_child(list, 0));
    }
}

// --- record view -------------------------------------------------------

static lv_obj_t *rec_time_lbl = NULL;
static lv_obj_t *rec_size_lbl = NULL;
static lv_obj_t *rec_bar = NULL;

// Stop the active recording and discard the file if it's too short to be
// useful (< 2 s). Capture the elapsed time before hw_rec_stop() — once
// stopped, hw_rec_elapsed_ms() returns 0. Always clears pending_rec_path
// so a stale path can't be deleted on a later teardown.
static constexpr uint32_t kMinRecMs = 2000;
static void finalize_recording()
{
    if (!hw_rec_running()) {
        pending_rec_path.clear();
        return;
    }
    uint32_t elapsed = hw_rec_elapsed_ms();
    hw_rec_stop();
    if (elapsed < kMinRecMs && !pending_rec_path.empty()) {
        hw_delete_file(pending_rec_path.c_str());
    }
    pending_rec_path.clear();
}

static void exit_to_menu()
{
    finalize_recording();
    hw_set_play_stop();
    menu_show_deferred();
}

// Record view only exists in quick mode, so stopping always exits the app.
static void stop_rec_and_return(lv_event_t *e)
{
    exit_to_menu();
}

static void rec_tick_cb(lv_timer_t *t)
{
    if (!rec_time_lbl) return;
    uint32_t ms = hw_rec_elapsed_ms();
    uint32_t secs = ms / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02u:%02u",
             (unsigned)(secs / 60), (unsigned)(secs % 60));
    lv_label_set_text(rec_time_lbl, buf);

    if (rec_bar) {
        lv_bar_set_value(rec_bar, (int32_t)secs, LV_ANIM_OFF);
    }

    if (rec_size_lbl) {
        uint32_t bytes = hw_rec_bytes_written();
        char sbuf[48];
        snprintf(sbuf, sizeof(sbuf), "max %u:%02u  -  %u KB",
                 (unsigned)(HW_REC_MAX_MS / 60000),
                 (unsigned)((HW_REC_MAX_MS / 1000) % 60),
                 (unsigned)(bytes / 1024));
        lv_label_set_text(rec_size_lbl, sbuf);
    }

    if (!hw_rec_running()) {
        // Auto-stop (cap hit) — quick mode exits the app.
        exit_to_menu();
    }
}

static void rec_dot_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void build_record_view()
{
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(main_page, 8, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);
    lv_obj_remove_flag(main_page, LV_OBJ_FLAG_SCROLLABLE);

    // Header: pulsing red dot + "REC".
    lv_obj_t *hdr = lv_obj_create(main_page);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *rec_dot = lv_obj_create(hdr);
    lv_obj_remove_style_all(rec_dot);
    lv_obj_set_size(rec_dot, 14, 14);
    lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_dot, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(rec_dot, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_shadow_width(rec_dot, 10, 0);
    lv_obj_set_style_shadow_opa(rec_dot, LV_OPA_50, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rec_dot);
    lv_anim_set_exec_cb(&a, rec_dot_anim_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_time(&a, 600);
    lv_anim_set_playback_time(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_obj_t *rec_status_lbl = lv_label_create(hdr);
    lv_label_set_text(rec_status_lbl, "REC");
    lv_obj_set_style_text_color(rec_status_lbl, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(rec_status_lbl, &lv_font_montserrat_20, 0);

    // Large centered elapsed time — fixed font so it never outgrows the page.
    rec_time_lbl = lv_label_create(main_page);
    lv_label_set_text(rec_time_lbl, "00:00");
    lv_obj_set_style_text_color(rec_time_lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(rec_time_lbl, &font_montserrat_48_subset, 0);

    // Progress bar: fraction of HW_REC_MAX_MS used.
    rec_bar = lv_bar_create(main_page);
    lv_obj_set_width(rec_bar, LV_PCT(80));
    lv_obj_set_height(rec_bar, 6);
    lv_bar_set_range(rec_bar, 0, (int32_t)(HW_REC_MAX_MS / 1000));
    lv_bar_set_value(rec_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(rec_bar, UI_COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rec_bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_radius(rec_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(rec_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rec_bar, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(rec_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(rec_bar, 3, LV_PART_INDICATOR);

    // Secondary line: cap and bytes written.
    rec_size_lbl = lv_label_create(main_page);
    char ibuf[48];
    snprintf(ibuf, sizeof(ibuf), "max %u:%02u  -  0 KB",
             (unsigned)(HW_REC_MAX_MS / 60000),
             (unsigned)((HW_REC_MAX_MS / 1000) % 60));
    lv_label_set_text(rec_size_lbl, ibuf);
    lv_obj_set_style_text_color(rec_size_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(rec_size_lbl, get_small_font(), 0);

    // Prominent red stop button.
    lv_obj_t *stop_btn = lv_btn_create(main_page);
    lv_obj_set_width(stop_btn, LV_PCT(80));
    lv_obj_set_height(stop_btn, 44);
    lv_obj_set_style_bg_color(stop_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_radius(stop_btn, UI_RADIUS, 0);
    lv_obj_set_style_shadow_color(stop_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_shadow_width(stop_btn, 18, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_opa(stop_btn, LV_OPA_40, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(stop_btn, stop_rec_and_return, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_lbl = lv_label_create(stop_btn);
    lv_label_set_text(stop_lbl, LV_SYMBOL_STOP "  Stop");
    lv_obj_set_style_text_color(stop_lbl, UI_COLOR_ON_ACCENT, 0);
    lv_obj_set_style_text_font(stop_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(stop_lbl);
    lv_group_add_obj(lv_group_get_default(), stop_btn);
    lv_group_focus_obj(stop_btn);

    if (!tick_timer) {
        tick_timer = lv_timer_create(rec_tick_cb, 200, NULL);
    }
}

// --- playback view -----------------------------------------------------

static lv_obj_t *pb_status_lbl = NULL;
static bool      pb_was_playing = false;

static void pb_tick_cb(lv_timer_t *t)
{
    if (!pb_status_lbl) return;
    bool running = hw_player_running();
    if (pb_was_playing && !running) {
        lv_label_set_text(pb_status_lbl, "Ended");
    }
    pb_was_playing = running;
}

static void pb_play_cb(lv_event_t *e)
{
    if (selected_path.empty()) return;
    // Filename without leading '/', matching hw_set_sd_music_play's convention.
    const char *fn = selected_path.c_str();
    if (*fn == '/') ++fn;
    hw_set_sd_music_play(fn);
    if (pb_status_lbl) lv_label_set_text(pb_status_lbl, LV_SYMBOL_PLAY "  Playing...");
    pb_was_playing = true;
    if (!tick_timer) {
        tick_timer = lv_timer_create(pb_tick_cb, 250, NULL);
    }
}

static void pb_stop_cb(lv_event_t *e)
{
    hw_set_play_stop();
    if (pb_status_lbl) lv_label_set_text(pb_status_lbl, "Stopped");
    pb_was_playing = false;
}

static lv_obj_t *pb_vol_pct_lbl = NULL;

static void pb_update_vol_label(int v)
{
    if (!pb_vol_pct_lbl) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(pb_vol_pct_lbl, buf);
}

static void pb_volume_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    hw_set_volume((uint8_t)v);
    pb_update_vol_label(v);
}

static void pb_delete_cb(lv_event_t *e)
{
    hw_set_play_stop();
    if (!selected_path.empty()) {
        hw_delete_file(selected_path.c_str());
        // Drop it from the in-memory list immediately — enter_list() below
        // kicks an async rescan (P3.3) rather than blocking here, and we
        // don't want the just-deleted file to flash back up in the list
        // while that rescan is still in flight.
        notes.erase(std::remove_if(notes.begin(), notes.end(),
                    [&](const Note &n) { return n.full_path == selected_path; }),
                    notes.end());
    }
    selected_path.clear();
    enter_list();
}

static void pb_back_cb(lv_event_t *e)
{
    hw_set_play_stop();
    enter_list();
}

static void build_playback_view()
{
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_page, 12, 0);
    lv_obj_set_style_pad_row(main_page, 8, 0);

    lv_obj_t *name_lbl = lv_label_create(main_page);
    lv_label_set_text(name_lbl, selected_name.c_str());
    lv_obj_set_width(name_lbl, LV_PCT(100));
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name_lbl, UI_COLOR_FG, 0);

    lv_obj_t *meta_lbl = lv_label_create(main_page);
    std::string meta = format_duration_s(bytes_to_seconds(selected_bytes))
                     + "   " + format_date(selected_mtime);
    lv_label_set_text(meta_lbl, meta.c_str());
    lv_obj_set_style_text_color(meta_lbl, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(meta_lbl, get_small_font(), 0);

    pb_status_lbl = lv_label_create(main_page);
    lv_label_set_text(pb_status_lbl, "Ready");
    lv_obj_set_style_text_color(pb_status_lbl, UI_COLOR_ACCENT, 0);

    // Volume row: icon + slider.
    lv_obj_t *vol_row = lv_obj_create(main_page);
    lv_obj_set_width(vol_row, LV_PCT(100));
    lv_obj_set_height(vol_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(vol_row, 0, 0);
    lv_obj_set_style_pad_column(vol_row, 10, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *vol_icon = lv_label_create(vol_row);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vol_icon, UI_COLOR_FG, 0);

    uint8_t cur_vol = hw_get_volume();

    lv_obj_t *pb_vol_slider = lv_slider_create(vol_row);
    lv_obj_set_flex_grow(pb_vol_slider, 1);
    lv_slider_set_range(pb_vol_slider, 0, 100);
    lv_slider_set_value(pb_vol_slider, cur_vol, LV_ANIM_OFF);
    // Tall, high-contrast bar so up/down changes are obvious.
    lv_obj_set_style_height(pb_vol_slider, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pb_vol_slider, UI_COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pb_vol_slider, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(pb_vol_slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pb_vol_slider, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(pb_vol_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(pb_vol_slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_size(pb_vol_slider, 20, 20, LV_PART_KNOB);
    lv_obj_set_style_bg_color(pb_vol_slider, UI_COLOR_FG, LV_PART_KNOB);
    lv_obj_set_style_radius(pb_vol_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    // Highlight the whole bar when focused so the user sees they're in it.
    lv_obj_set_style_outline_width(pb_vol_slider, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(pb_vol_slider, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(pb_vol_slider, 2, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(pb_vol_slider, pb_volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(lv_group_get_default(), pb_vol_slider);

    pb_vol_pct_lbl = lv_label_create(vol_row);
    lv_obj_set_style_text_color(pb_vol_pct_lbl, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(pb_vol_pct_lbl, get_small_font(), 0);
    lv_obj_set_style_min_width(pb_vol_pct_lbl, 40, 0);
    lv_obj_set_style_text_align(pb_vol_pct_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    pb_update_vol_label(cur_vol);

    // Button row.
    lv_obj_t *row = lv_obj_create(main_page);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto make_btn = [&](const char *icon, const char *txt, lv_event_cb_t cb,
                        lv_color_t bg) -> lv_obj_t * {
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_style_bg_color(btn, bg, 0);
        lv_obj_set_style_radius(btn, UI_RADIUS, 0);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "%s %s", icon, txt);
        lv_obj_set_style_text_color(lbl, UI_COLOR_ON_ACCENT, 0);
        lv_obj_center(lbl);
        lv_group_add_obj(lv_group_get_default(), btn);
        return btn;
    };

    lv_obj_t *play_btn = make_btn(LV_SYMBOL_PLAY, "Play", pb_play_cb, UI_COLOR_ACCENT);
    make_btn(LV_SYMBOL_STOP, "Stop", pb_stop_cb, lv_palette_main(LV_PALETTE_GREY));
    make_btn(LV_SYMBOL_TRASH, "Del",  pb_delete_cb, lv_palette_main(LV_PALETTE_RED));
    make_btn(LV_SYMBOL_LEFT, "Back",  pb_back_cb, lv_palette_main(LV_PALETTE_GREY));

    lv_group_focus_obj(play_btn);
}

// --- view transitions --------------------------------------------------

static void kill_tick_timer()
{
    if (tick_timer) {
        lv_timer_del(tick_timer);
        tick_timer = NULL;
    }
}

static void clear_view_refs()
{
    rec_time_lbl = NULL;
    rec_size_lbl = NULL;
    rec_bar = NULL;
    pb_status_lbl = NULL;
    pb_vol_pct_lbl = NULL;
}

static void render_view()
{
    if (!main_page) return;
    lv_obj_clean(main_page);
    clear_view_refs();
    kill_tick_timer();

    switch (current_view) {
    case VIEW_LIST:     build_list_view(); break;
    case VIEW_RECORD:   build_record_view(); break;
    case VIEW_PLAYBACK: build_playback_view(); break;
    }
}

static void enter_list()
{
    show_list_view();
}

static void enter_playback(const Note &n)
{
    selected_path   = n.full_path;
    selected_name   = n.filename;
    selected_bytes  = n.data_bytes;
    selected_mtime  = n.mtime;
    current_view    = VIEW_PLAYBACK;
    render_view();
}

// --- lifecycle ---------------------------------------------------------

// In quick-record (home shortcut) every back press exits the app. In the
// browse-only flow (Settings → Recordings) the list view is the root, so
// VIEW_PLAYBACK pops back to it and VIEW_LIST exits. Shared by the status-bar
// back button and the touch quit button.
static void do_back()
{
    if (!s_quick_record && current_view == VIEW_PLAYBACK) {
        hw_set_play_stop();
        enter_list();
        return;
    }
    exit_to_menu();
}

static void exit_btn_cb(lv_event_t *e)
{
    do_back();
}

static void ui_notes_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    enable_keyboard();

    ensure_notes_dir();
    selected_path.clear();
    selected_name.clear();
    selected_bytes = 0;
    selected_mtime = 0;

    // Pick the initial view.
    //   - Quick-record (home shortcut) jumps straight into a fresh recording
    //     when the mic is available; falls back to the browse list if the
    //     mic is missing or hw_rec_start fails, so the user sees something
    //     instead of a silent failure.
    //   - Otherwise (Settings → Recordings) opens directly on the list.
    if (s_quick_record && hw_mic_available()) {
        if (hw_player_running()) hw_set_play_stop();
        pending_rec_path = build_new_note_path();
        if (hw_rec_start(pending_rec_path.c_str())) {
            current_view = VIEW_RECORD;
        } else {
            s_quick_record = false;
            current_view = VIEW_LIST;
        }
    } else {
        if (s_quick_record) s_quick_record = false;  // no mic — drop quick mode
        current_view = VIEW_LIST;
    }

    menu = create_menu(parent);
    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    ui_show_back_button(exit_btn_cb);

    lv_menu_set_page(menu, main_page);

    if (current_view == VIEW_LIST) {
        show_list_view();
    } else {
        render_view();
    }

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button(exit_btn_cb, NULL);
#endif
}

static void ui_notes_exit()
{
    ui_hide_back_button();
    disable_keyboard();
    kill_tick_timer();
    finalize_recording();
    hw_set_play_stop();
    // Detach the bg scan, if any. A running worker is left to finish on its
    // own (it never touches the UI tree); its result is freed on arrival
    // unless kick_notes_reload() re-attaches it on a later re-entry.
    s_notes_scan_job.teardown();
    s_notes_scan_pending = false;
    ui_app_widgets_del(&menu, &quit_btn);
    main_page = NULL;
    clear_view_refs();
    notes.clear();
    selected_path.clear();
    selected_name.clear();
}

} // namespace

namespace apps {
class AudioNotesApp : public core::App {
public:
    AudioNotesApp(const char *name, bool quick_record)
        : core::App(name), m_quick_record(quick_record) {}
    void onStart(lv_obj_t *parent) override {
        s_quick_record = m_quick_record;
        ui_notes_enter(parent);
    }
    void onStop() override {
        ui_notes_exit();
        core::App::onStop();
    }
private:
    bool m_quick_record;
};

std::shared_ptr<core::App> make_audio_notes_app() {
    // Home-screen shortcut — jumps straight into recording.
    return std::make_shared<AudioNotesApp>("Recorder", true);
}
std::shared_ptr<core::App> make_audio_recordings_app() {
    // Settings → Recordings — opens the notes list.
    return std::make_shared<AudioNotesApp>("Recordings", false);
}
} // namespace apps
