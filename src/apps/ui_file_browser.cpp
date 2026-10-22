/**
 * @file      ui_file_browser.cpp
 * @brief     File browser with source selection, extension filter,
 *            and folder listing.
 *
 * State reset on onStop (ui_files_exit):
 *   widgets: menu, parent_obj, file_list, quit_btn, main_page,
 *            src_btn_int, src_btn_sd,
 *            filter_btn_txt / _non_txt / _all   — destroyed via lv_obj_del
 *                                                  then nulled
 *   job:     s_list_job (directory listing worker, P7.19)
 *                                              — teardown(): drain stopped,
 *                                                a running worker detached
 *   timers:  s_loading_timer (150 ms loader delay) — lv_timer_delete + null
 *   modal:   s_loading (ui_loading_t)            — ui_loading_close
 * No hardware sessions are held here. If you add cached state, list it
 * above AND kill it in the exit path — a leaked pointer to a destroyed
 * widget crashes on next entry.
 */
#include "../ui_define.h"
#include "app_registry.h"
#include "../core/app_manager.h"
#include "../core/one_shot_job.h"
#include "../hal/system.h"
#include "../hal/storage.h"
#include "../hal/notes_crypto.h"
#include "../hal/notes_path.h"
#include <vector>
#include <string>
#include <algorithm>

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_10);

namespace {

enum StorageSource {
    SOURCE_INTERNAL,
    SOURCE_SD
};

enum FileFilter {
    FILTER_TXT,      // show only .txt files (+ folders)
    FILTER_NON_TXT,  // show every non-.txt file (+ folders)
    FILTER_ALL       // show every file (+ folders)
};

static inline bool ends_with_ci(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (b >= 'A' && b <= 'Z') b = b - 'A' + 'a';
        if (a != b) return false;
    }
    return true;
}

static lv_obj_t *menu = NULL;
static lv_obj_t *parent_obj = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *main_page = NULL;
static lv_obj_t *src_btn_int = NULL;
static lv_obj_t *src_btn_sd = NULL;
static lv_obj_t *filter_btn_txt = NULL;
static lv_obj_t *filter_btn_non_txt = NULL;
static lv_obj_t *filter_btn_all = NULL;

static StorageSource current_source = SOURCE_INTERNAL;
static FileFilter current_filter = FILTER_TXT;
static std::string current_dir = "/";

// Raw directory listing as read from disk — source/dir scoped and
// filter-independent (the HAL is called with filter_ext=nullptr). Cached so a
// filter-button toggle re-filters in place instead of re-scanning the disk
// (which, on SD, holds the SPI lock for the whole openNextFile walk).
static std::vector<HwDirEntry> raw_cache;
// Filtered + sorted view of raw_cache that the list renders. Each row button's
// user_data is its index here + 1 (0 = the ".." row / not an entry row).
static std::vector<HwDirEntry> all_entries;

// Cap on rendered rows: each entry spawns ~3 LVGL objects, and a huge dir would
// otherwise freeze the UI (and stall the SPI-lock-held flush) building them all.
// Entries are dir-first then mtime-desc sorted, so the cap keeps the newest.
static const int kMaxRenderedRows = 200;

static std::string join_path(const std::string &dir, const std::string &leaf)
{
    if (dir.empty() || dir == "/") return "/" + leaf;
    if (!dir.empty() && dir.back() == '/') return dir + leaf;
    return dir + "/" + leaf;
}

static std::string parent_of(const std::string &dir)
{
    if (dir.empty() || dir == "/") return "/";
    std::string d = dir;
    if (d.back() == '/') d.pop_back();
    size_t slash = d.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return d.substr(0, slash);
}

// Rebuild the display list from the cached raw listing, applying the current
// filter + sort. No disk access — this is what the filter buttons call.
static void apply_filter()
{
    all_entries.clear();

    for (const auto &r : raw_cache) {
        // Hide tasks.txt bookkeeping file from the browser.
        if (!r.is_dir && notes_path_is_tasks_file(r.path.c_str())) continue;

        // Apply UI-side filter to files; folders are always shown.
        if (!r.is_dir) {
            bool is_txt = ends_with_ci(r.path, ".txt");
            if (current_filter == FILTER_TXT && !is_txt) continue;
            if (current_filter == FILTER_NON_TXT && is_txt) continue;
        }
        all_entries.push_back(r);
    }

    std::sort(all_entries.begin(), all_entries.end(),
              [](const HwDirEntry &a, const HwDirEntry &b) {
                  if (a.is_dir != b.is_dir) return a.is_dir;
                  if (a.mtime != b.mtime) return a.mtime > b.mtime;
                  return a.path < b.path;
              });
}

// P7.19: directory listings run on a worker (SD walks hold the SPI lock at
// 4 MHz, and the HAL may wait for a background note save), not inside the
// LVGL click callback. The requested (source, dir) is only committed to
// current_source/current_dir when its listing lands, so rows of the list on
// screen keep resolving against the dir they came from.
struct ListResult {
    StorageSource src;
    std::string dir;
    std::vector<HwDirEntry> entries;
};

static void list_done(ListResult &r);
static core::OneShotJob<ListResult> s_list_job(list_done);
static StorageSource want_source = SOURCE_INTERNAL;
static std::string want_dir = "/";

// The loader only appears when a listing is still running after this long,
// so a fast FFat listing doesn't flash a modal.
static const uint32_t kLoaderDelayMs = 150;
static ui_loading_t s_loading = {};
static bool s_loading_open = false;
static lv_timer_t *s_loading_timer = NULL;

static void list_into(StorageSource src, const std::string &dir,
                      std::vector<HwDirEntry> &out)
{
    if (src == SOURCE_INTERNAL) {
        hw_list_internal_entries(out, nullptr, dir.c_str());
    } else {
        hw_list_sd_entries(out, nullptr, dir.c_str());
    }
}

static void loader_hide()
{
    if (s_loading_timer) {
        lv_timer_delete(s_loading_timer);
        s_loading_timer = NULL;
    }
    if (s_loading_open) {
        ui_loading_close(&s_loading);
        s_loading_open = false;
    }
}

static void loader_timer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_loading_timer = NULL;
    if (!menu || s_loading_open || !s_list_job.busy()) return;
    ui_loading_open(&s_loading, "Files", "Reading folder...");
    s_loading_open = true;
}

// Scan (src, dir) into raw_cache, then rebuild the display list. Call on
// source/dir change and after mutations (delete); filter toggles use
// apply_filter() to avoid a redundant re-scan of the same directory. A call
// while a listing runs re-targets it: list_done re-kicks for the newest target.
static void kick_load(StorageSource src, const std::string &dir)
{
    want_source = src;
    want_dir = dir;
    using Job = core::OneShotJob<ListResult>;
    // 4 KB: one directory walk into a vector, no TLS (same as notes_scan).
    Job::Start st = s_list_job.start([src, dir]() {
        ListResult *r = new ListResult{src, dir, {}};
        list_into(src, dir, r->entries);
        return r;
    }, 4096, "files_list", 1);
    if (st == Job::Start::Failed) {
        ListResult r{src, dir, {}};   // no task: list inline, as before P7.19
        list_into(src, dir, r.entries);
        list_done(r);
        return;
    }
    if (s_list_job.busy() && !s_loading_open && !s_loading_timer) {
        s_loading_timer = lv_timer_create(loader_timer_cb, kLoaderDelayMs, NULL);
    }
}

static std::string format_size(uint32_t bytes)
{
    char buf[8];
    if (bytes < 1024u) {
        snprintf(buf, sizeof(buf), "%uB", (unsigned)bytes);
    } else if (bytes < 1024u * 1024u) {
        unsigned kb10 = (bytes * 10u + 512u) / 1024u;
        if (kb10 < 100u) snprintf(buf, sizeof(buf), "%u.%uK", kb10 / 10u, kb10 % 10u);
        else             snprintf(buf, sizeof(buf), "%uK", (bytes + 512u) / 1024u);
    } else {
        unsigned mb10 = (bytes * 10u + (1u << 19)) / (1u << 20);
        if (mb10 < 100u) snprintf(buf, sizeof(buf), "%u.%uM", mb10 / 10u, mb10 % 10u);
        else             snprintf(buf, sizeof(buf), "%uM", (bytes + (1u << 19)) / (1u << 20));
    }
    return std::string(buf);
}

static std::string display_name(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

static void refresh_ui();

// The entry a list row stands for, or nullptr for the ".." row and anything
// that isn't a row of file_list.
static const HwDirEntry *row_entry(lv_obj_t *btn)
{
    if (!btn || lv_obj_get_parent(btn) != file_list) return nullptr;
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn) - 1;
    if (idx < 0 || idx >= (intptr_t)all_entries.size()) return nullptr;
    return &all_entries[idx];
}

static void file_click_open_now(const std::string &path)
{
    // IS.5: don't tear the browser down under the row being clicked.
    ui_text_editor_open_file_deferred(path.c_str());
}

static void file_click_after_unlock(bool ok, void *ud)
{
    std::string *p = (std::string *)ud;
    if (ok && p) file_click_open_now(*p);
    delete p;
}

static void file_click_cb(lv_event_t *e)
{
    const HwDirEntry *ent = row_entry((lv_obj_t *)lv_event_get_target(e));
    if (!ent) return;

    std::string path = join_path(current_dir, ent->path);
    if (path[0] != '/') path = "/" + path;

    /* Gate the open behind the unlock modal when encryption is enabled and
     * the current session hasn't been unlocked yet. Decryption in the editor
     * would otherwise produce an empty file. */
    if (notes_crypto_is_enabled() && !notes_crypto_is_unlocked() &&
        notes_crypto_path_is_protected(path.c_str())) {
        ui_passphrase_unlock(file_click_after_unlock, new std::string(path));
        return;
    }

    file_click_open_now(path);
}

static void folder_click_cb(lv_event_t *e)
{
    const HwDirEntry *ent = row_entry((lv_obj_t *)lv_event_get_target(e));
    if (!ent) return;
    kick_load(current_source, join_path(current_dir, ent->path));
}

static void parent_click_cb(lv_event_t *e)
{
    kick_load(current_source, parent_of(current_dir));
}

static void style_toggle_btn(lv_obj_t *btn, bool active)
{
    if (!btn) return;
    if (active) {
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, UI_COLOR_ON_ACCENT, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, UI_COLOR_MUTED, LV_PART_MAIN);
    }
}

void ui_file_browser_enter(lv_obj_t *parent);
void ui_file_browser_exit(lv_obj_t *parent);

static void refresh_ui()
{
    if (!file_list) return;
    lv_obj_clean(file_list);

    int total = (int)all_entries.size();
    int render_count = total > kMaxRenderedRows ? kMaxRenderedRows : total;
    bool show_parent = (current_dir != "/" && !current_dir.empty());

    style_toggle_btn(src_btn_int,        current_source == SOURCE_INTERNAL);
    style_toggle_btn(src_btn_sd,         current_source == SOURCE_SD);
    style_toggle_btn(filter_btn_txt,     current_filter == FILTER_TXT);
    style_toggle_btn(filter_btn_non_txt, current_filter == FILTER_NON_TXT);
    style_toggle_btn(filter_btn_all,     current_filter == FILTER_ALL);

    if (show_parent) {
        lv_obj_t *btn = lv_list_add_btn(file_list, LV_SYMBOL_LEFT, "..");
        lv_obj_add_event_cb(btn, parent_click_cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(lv_group_get_default(), btn);
    }

    if (total == 0 && !show_parent) {
        lv_obj_t *empty = lv_label_create(file_list);
        lv_label_set_text(empty, LV_SYMBOL_WARNING "  No items");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
    } else {
        for (int i = 0; i < render_count; ++i) {
            const HwDirEntry &ent = all_entries[i];
            std::string label = display_name(ent.path);
            const char *icon = ent.is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            // Trailing column: size (files only).
            std::string size = ent.is_dir ? std::string() : format_size(ent.size);
            lv_obj_t *btn = ui_list_add_meta_row(file_list, icon, label.c_str(),
                                                 ent.is_dir ? nullptr : size.c_str(),
                                                 &lv_font_montserrat_10);
            lv_obj_set_style_text_align(lv_obj_get_child(btn, 0), LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_user_data(btn, (void *)(intptr_t)(i + 1));
            lv_obj_add_event_cb(btn, ent.is_dir ? folder_click_cb : file_click_cb,
                                LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), btn);
        }

        // Non-focusable footer when the directory exceeds the render cap.
        if (total > render_count) {
            lv_obj_t *more = lv_label_create(file_list);
            lv_label_set_text_fmt(more, "%d more (narrow the filter)",
                                  total - render_count);
            lv_obj_set_style_text_color(more, UI_COLOR_MUTED, 0);
            lv_obj_set_style_text_align(more, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(more, LV_PCT(100));
            lv_obj_set_style_pad_all(more, 8, 0);
        }
    }

    lv_obj_t *to_focus = NULL;
    if ((total > 0 || show_parent) && lv_obj_get_child_count(file_list) > 0) {
        to_focus = lv_obj_get_child(file_list, 0);
    }
    if (to_focus) {
        lv_group_focus_obj(to_focus);
    } else if (src_btn_int) {
        lv_group_focus_obj(current_source == SOURCE_INTERNAL ? src_btn_int : src_btn_sd);
    }
}

static void source_click_cb(lv_event_t *e)
{
    StorageSource s = (StorageSource)(uintptr_t)lv_event_get_user_data(e);
    if (current_source != s) {
        kick_load(s, "/");   // list_done commits the source and refreshes
        return;
    }
    refresh_ui();
}

// LVGL thread (OneShotJob drain, or inline on the emulator / task failure).
static void list_done(ListResult &r)
{
    if (!menu) return;
    if (r.src != want_source || r.dir != want_dir) {
        kick_load(want_source, want_dir);   // a newer target was picked meanwhile
        return;
    }
    loader_hide();
    current_source = r.src;
    current_dir = r.dir;
    raw_cache = std::move(r.entries);
    apply_filter();
    refresh_ui();
}

static void filter_click_cb(lv_event_t *e)
{
    FileFilter f = (FileFilter)(uintptr_t)lv_event_get_user_data(e);
    if (current_filter != f) {
        current_filter = f;
        apply_filter();   // re-filter the cached listing; no disk re-scan
    }
    refresh_ui();
}

static void do_exit()
{
    menu_show_deferred();  // onStop runs ui_file_browser_exit
}

static void exit_btn_cb(lv_event_t *e)
{
    do_exit();
}

static lv_obj_t *make_side_btn(lv_obj_t *parent, const char *icon, const char *txt,
                               lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(btn, UI_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    if (icon && icon[0]) {
        lv_label_set_text_fmt(lbl, "%s %s", icon, txt);
    } else {
        lv_label_set_text(lbl, txt);
    }
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_group_add_obj(lv_group_get_default(), btn);
    return btn;
}

static lv_obj_t *make_divider(lv_obj_t *parent)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_width(line, LV_PCT(70));
    lv_obj_set_height(line, 1);
    lv_obj_set_style_bg_color(line, UI_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_margin_ver(line, 2, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

struct DeleteCtx {
    std::string path;
    bool is_dir;
    bool use_sd;
};

static void delete_confirm_cb(bool confirmed, void *ud)
{
    DeleteCtx *ctx = (DeleteCtx *)ud;

    if (confirmed && ctx) {
        if (hw_delete_path(ctx->path.c_str(), ctx->use_sd)) {
            kick_load(current_source, current_dir);
        } else {
            ui_msg_pop_up("Error", ctx->is_dir ? "Failed to delete folder."
                                               : "Failed to delete file.");
        }
    }

    delete ctx;
}

static void show_delete_confirm(const std::string &path, bool is_dir)
{
    char msg[200];
    if (is_dir) {
        snprintf(msg, sizeof(msg), "Delete folder and all contents?\n%s", path.c_str());
    } else {
        snprintf(msg, sizeof(msg), "Delete this file?\n%s", path.c_str());
    }

    DeleteCtx *ctx = new DeleteCtx{path, is_dir, current_source == SOURCE_SD};
    ui_confirm("Confirm", msg, delete_confirm_cb, ctx);
}

static void file_list_key_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_BACKSPACE && key != 'd' && key != 'D') return;

    lv_group_t *g = lv_group_get_default();
    lv_obj_t *focused = lv_group_get_focused(g);
    if (!focused) return;

    const HwDirEntry *ent = row_entry(focused);
    if (!ent) return;
    show_delete_confirm(join_path(current_dir, ent->path), ent->is_dir);
}

void ui_file_browser_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    parent_obj = parent;
    enable_keyboard();

    bool sd_online = (HW_SD_ONLINE & hw_get_device_online()) != 0;
    current_source = SOURCE_INTERNAL;
    current_dir = "/";

    menu = create_menu(parent, nullptr);

    main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    ui_show_back_button(exit_btn_cb);

    // Body: two frames side-by-side — left sidebar (controls), right file list.
    lv_obj_t *body = lv_obj_create(main_page);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_column(body, 6, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // Left sidebar: storage + filter stacked vertically.
    lv_obj_t *sidebar = lv_obj_create(body);
    lv_obj_set_width(sidebar, LV_PCT(22));
    lv_obj_set_height(sidebar, LV_PCT(100));
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sidebar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(sidebar, 4, 0);
    lv_obj_set_style_pad_row(sidebar, 3, 0);
    lv_obj_set_style_border_width(sidebar, 1, 0);
    lv_obj_set_style_border_color(sidebar, UI_COLOR_MUTED, 0);
    lv_obj_set_style_radius(sidebar, UI_RADIUS, 0);
    lv_obj_set_style_bg_opa(sidebar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(sidebar, &lv_font_montserrat_14, 0);

    src_btn_int = make_side_btn(sidebar, LV_SYMBOL_DRIVE, "Internal",
                                source_click_cb, (void *)(uintptr_t)SOURCE_INTERNAL);
    if (sd_online) {
        src_btn_sd = make_side_btn(sidebar, LV_SYMBOL_SD_CARD, "SD Card",
                                   source_click_cb, (void *)(uintptr_t)SOURCE_SD);
    } else {
        src_btn_sd = NULL;
    }

    make_divider(sidebar);
    filter_btn_txt = make_side_btn(sidebar, NULL, ".txt only",
                                   filter_click_cb, (void *)(uintptr_t)FILTER_TXT);
    filter_btn_non_txt = make_side_btn(sidebar, NULL, "No .txt",
                                       filter_click_cb, (void *)(uintptr_t)FILTER_NON_TXT);
    filter_btn_all = make_side_btn(sidebar, NULL, "All",
                                   filter_click_cb, (void *)(uintptr_t)FILTER_ALL);

    // Right side: file list.
    file_list = lv_list_create(body);
    lv_obj_set_flex_grow(file_list, 1);
    lv_obj_set_height(file_list, LV_PCT(100));
    lv_obj_set_style_radius(file_list, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(file_list, 2, 0);
    lv_obj_add_event_cb(file_list, file_list_key_cb, LV_EVENT_KEY, NULL);

    kick_load(current_source, current_dir);   // inline on the emulator

    lv_menu_set_page(menu, main_page);

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        do_exit();
    }, NULL);
#endif
}

void ui_file_browser_exit(lv_obj_t *parent)
{
    // Safe while a listing runs: teardown() detaches the worker, which then
    // frees its own result; it never touches LVGL.
    loader_hide();
    s_list_job.teardown();
    ui_hide_back_button();
    disable_keyboard();
    if (menu) {
        lv_obj_clean(menu);
        lv_obj_del(menu);
        menu = NULL;
    }
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    file_list = NULL;
    main_page = NULL;
    src_btn_int = NULL;
    src_btn_sd = NULL;
    filter_btn_txt = NULL;
    filter_btn_non_txt = NULL;
    filter_btn_all = NULL;
    all_entries.clear();
    raw_cache.clear();
}

} // namespace

namespace apps {
class FileBrowserApp : public core::App {
public:
    FileBrowserApp() : core::App("Files") {}
    void onStart(lv_obj_t *parent) override { ui_file_browser_enter(parent); }
    void onStop() override {
        ui_file_browser_exit(getRoot());
        core::App::onStop();
    }
};

APP_FACTORY(make_file_browser_app, FileBrowserApp)
} // namespace apps
