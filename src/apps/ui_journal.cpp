/**
 * @file      ui_journal.cpp
 * @brief     Chronological view of internal files with timestamps in blog style.
 *
 * State reset on onStop (ui_journal_exit):
 *   widgets: menu, quit_btn            — destroyed via lv_obj_del then nulled
 *            parent_obj, pending_journal_parent
 *                                       — nulled (not owned)
 *   paging:  target_focus_index, current_page — reset
 *   cache:   journal_entries / cache_valid    — kept on purpose (warm reopen)
 *   timers:  bg_refresh_timer           — the deferred one-shot that starts the
 *                                         rescan when the FS changed; also holds
 *                                         the 0 ms first-run one-shot
 *                                         (first_run_timer_cb, P7.6); both are
 *                                         cancelled via cancel_bg_refresh() on
 *                                         exit/rebuild.
 *   tasks:   s_bg_job (journal_bg)      — OneShotJob worker running the rescan on a
 *                                         snapshot; s_bg_job.teardown() on exit
 *                                         detaches it (it frees its own result).
 *                                         bg_refresh_done applies the result on the
 *                                         LVGL thread, dropped if s_entries_gen moved.
 *   flags:   s_first_run_pending        — true while a first-run reconcile
 *                                         (P7.6: index missing/empty/undecryptable,
 *                                         run on s_bg_job instead of the LVGL thread)
 *                                         owns the 'Loading Journal' loader;
 *                                         bg_refresh_done clears it and hides the
 *                                         loader; ui_journal_exit resets it to false.
 * No hardware sessions are held here. If you add any, list them above AND extend
 * the exit path.
 */
#include "../ui_define.h"
#include "app_registry.h"
#include "../core/app_manager.h"
#include "../hal/notes_crypto.h"
#include "../hal/notes_path.h"
#include "../core/one_shot_job.h"
#include "../core/input_profile.h"
#include "../hal/storage.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

struct JournalEntry {
    std::string filename;
    uint32_t mtime;
    std::string snippet;
    bool truncated;
};

constexpr const char *kIndexPath = "/journal_idx.bin";
constexpr uint32_t kIndexMagic = 0x4A524E4Cu; // 'JRNL' little-endian
constexpr uint8_t  kIndexVersion = 1;
/* Per-note preview cached in the (static, always-resident) journal index and
 * shown in the ~125 px scrollable box on the list. The whole index lives in
 * internal heap, so this directly gates how much RAM 100+ notes pin away from
 * WiFi/TLS. 1.5 KB is still several screenfuls in that box; the full note opens
 * in the editor on tap. The on-disk index is length-prefixed, so shrinking this
 * needs no migration — longer snippets from an older index stay valid until
 * their note is next re-scanned. */
constexpr size_t   kSnippetBytes = 1536;
#define JOURNAL_PAGE_SIZE 5

void write_u16le(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
}

void write_u32le(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
    out.push_back((uint8_t)((v >> 16) & 0xFF));
    out.push_back((uint8_t)((v >> 24) & 0xFF));
}

bool read_u16le(const std::vector<uint8_t> &in, size_t &pos, uint16_t &v)
{
    if (pos + 2 > in.size()) return false;
    v = (uint16_t)in[pos] | ((uint16_t)in[pos + 1] << 8);
    pos += 2;
    return true;
}

bool read_u32le(const std::vector<uint8_t> &in, size_t &pos, uint32_t &v)
{
    if (pos + 4 > in.size()) return false;
    v = (uint32_t)in[pos] |
        ((uint32_t)in[pos + 1] << 8) |
        ((uint32_t)in[pos + 2] << 16) |
        ((uint32_t)in[pos + 3] << 24);
    pos += 4;
    return true;
}

bool load_index_file(std::vector<JournalEntry> &out)
{
    out.clear();
    std::vector<uint8_t> raw;
    if (!hw_read_preferred_bytes(kIndexPath, raw)) return false;

    /* Encrypted indices carry the OpenSSL Salted__ prefix. Decrypt in place
     * before the header checks below try to interpret the bytes. */
    if (content_has_salted_magic((const char *)raw.data(), raw.size())) {
        if (!notes_crypto_is_unlocked()) return false;
        std::string plain;
        if (!notes_crypto_decrypt_buffer(raw.data(), raw.size(), plain)) return false;
        raw.assign(plain.begin(), plain.end());
    }

    if (raw.size() < 9) return false;

    size_t pos = 0;
    uint32_t magic;
    if (!read_u32le(raw, pos, magic) || magic != kIndexMagic) return false;
    if (raw[pos++] != kIndexVersion) return false;
    uint32_t count;
    if (!read_u32le(raw, pos, count)) return false;

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        JournalEntry e;
        uint32_t mtime;
        if (!read_u32le(raw, pos, mtime)) return false;
        e.mtime = mtime;

        if (pos >= raw.size()) return false;
        uint8_t name_len = raw[pos++];
        if (pos + name_len > raw.size()) return false;
        e.filename.assign((const char *)&raw[pos], name_len);
        pos += name_len;

        uint16_t snip_len;
        if (!read_u16le(raw, pos, snip_len)) return false;
        if (pos + snip_len + 1 > raw.size()) return false;
        e.snippet.assign((const char *)&raw[pos], snip_len);
        pos += snip_len;
        e.truncated = (raw[pos++] != 0);

        out.push_back(std::move(e));
    }
    return true;
}

void save_index_file(const std::vector<JournalEntry> &entries)
{
    std::vector<uint8_t> buf;
    size_t est_size = 16;
    for (const auto &e : entries) {
        est_size += 8 + e.filename.size() + e.snippet.size();
    }
    buf.reserve(est_size);

    write_u32le(buf, kIndexMagic);
    buf.push_back(kIndexVersion);
    write_u32le(buf, (uint32_t)entries.size());
    for (const auto &e : entries) {
        write_u32le(buf, e.mtime);
        uint8_t name_len = (uint8_t)std::min<size_t>(e.filename.size(), 255);
        buf.push_back(name_len);
        buf.insert(buf.end(), e.filename.begin(), e.filename.begin() + name_len);
        uint16_t snip_len = (uint16_t)std::min<size_t>(e.snippet.size(), 65535);
        write_u16le(buf, snip_len);
        buf.insert(buf.end(), e.snippet.begin(), e.snippet.begin() + snip_len);
        buf.push_back(e.truncated ? 1 : 0);
    }

    /* The index caches the first kSnippetBytes (1.5 KB) of every note as a
     * plaintext snippet.
     * Without encryption that would defeat the at-rest protection on the
     * notes themselves, so we encrypt the whole blob with the same key. */
    if (notes_crypto_should_encrypt()) {
        std::vector<uint8_t> ct;
        if (notes_crypto_encrypt_buffer(buf.data(), buf.size(), ct)) {
            hw_save_preferred_bytes(kIndexPath, ct.data(), ct.size());
            // Re-pin the index key to the new salt (cached by the encrypt; no derive).
            notes_crypto_pin_key(NC_PIN_JOURNAL_INDEX, ct.data(), ct.size(), false);
            return;
        }
        /* Encryption failure falls through to the plaintext write so we don't
         * lose the index entirely on transient errors. */
    }
    hw_save_preferred_bytes(kIndexPath, buf.data(), buf.size());
}

using ProgressFn = void (*)(void *ud, const char *phase, int cur, int total, const char *detail);

static void report(ProgressFn cb, void *ud, const char *phase, int cur, int total, const char *detail)
{
    if (cb) {
        static uint32_t last_refr = 0;
        uint32_t now = lv_tick_get();
        cb(ud, phase, cur, total, detail);
        if (lv_tick_elaps(last_refr) > 100) {
            lv_refr_now(NULL);
            last_refr = now;
            // The scan/decrypt loop runs inside lv_timer_handler on the
            // dedicated LVGL task. Without a yield here, large journals keep
            // IDLE1 off the CPU long enough for the task watchdog to panic the
            // device. vTaskDelay(1) costs one tick but guarantees IDLE runs.
#ifdef ARDUINO
            vTaskDelay(1);
#endif
        }
    }
}

bool refresh_journal_entries(std::vector<JournalEntry> &entries, ProgressFn cb = nullptr, void *ud = nullptr)
{
    static ProgressFn g_cb = nullptr;
    static void *g_ud = nullptr;
    if (cb) {
        g_cb = cb;
        g_ud = ud;
    }

    report(cb, ud, "Scanning files...", 0, 0, nullptr);
    std::vector<std::pair<std::string, uint32_t>> current;
    // The worker passes no callback and must never reach g_cb (it is LVGL-thread
    // progress UI).
    if (cb) {
        hw_get_preferred_txt_files_info(current, [](int cur, int total, const char *name) {
            if (g_cb) g_cb(g_ud, "Scanning files...", cur, 0, name);
        });
    } else {
        hw_get_preferred_txt_files_info(current, nullptr);
    }

    current.erase(std::remove_if(current.begin(), current.end(),
                                 [](const std::pair<std::string, uint32_t> &p) {
                                     return notes_path_is_tasks_file(p.first.c_str());
                                 }),
                  current.end());

    std::sort(current.begin(), current.end(), [](const std::pair<std::string, uint32_t> &a,
                                                  const std::pair<std::string, uint32_t> &b) {
        return a.first > b.first;
    });

    report(cb, ud, "Scanning files...", (int)current.size(), (int)current.size(), nullptr);

    std::vector<JournalEntry> existing;
    if (entries.empty()) {
        report(cb, ud, "Loading index...", 0, 1, nullptr);
        load_index_file(existing);
        report(cb, ud, "Loading index...", 1, 1, nullptr);
    } else {
        existing = std::move(entries);
        entries.clear();
    }

    std::unordered_map<std::string, size_t> existing_map;
    for (size_t i = 0; i < existing.size(); ++i) {
        existing_map[existing[i].filename] = i;
    }

    std::vector<JournalEntry> next;
    next.reserve(current.size());
    bool dirty = (existing.size() != current.size());
    int total = (int)current.size();
    int done = 0;

    for (const auto &cur : current) {
        const std::string &name = cur.first;
        uint32_t mtime = cur.second;

        auto it = existing_map.find(name);
        bool needs_rebuild = true;
        
        if (it != existing_map.end()) {
            const JournalEntry &old_e = existing[it->second];
            if (old_e.mtime == mtime) {
                next.push_back(std::move(existing[it->second]));
                needs_rebuild = false;
            }
        }

        if (needs_rebuild) {
            report(cb, ud, "Reading previews...", done, total, name.c_str());
            JournalEntry e;
            e.filename = name;
            e.mtime = mtime;
            std::string snippet;
            bool truncated = false;
            hw_read_preferred_file_snippet(name.c_str(), snippet, kSnippetBytes, &truncated);
            e.snippet = std::move(snippet);
            e.truncated = truncated;
            next.push_back(std::move(e));
            dirty = true;
        }
        done++;
        if (total > 0 && cb) {
            report(cb, ud, needs_rebuild ? "Reading previews..." : "Processing...", done, total, name.c_str());
        }
#ifdef ARDUINO
        if (!cb) vTaskDelay(1);  // worker: let IDLE0 run between notes (PBKDF2/AES per note)
#endif
    }

    // `next` is already filename-descending: it was built by walking the
    // sorted `current` list above.

    if (dirty) {
        report(cb, ud, "Saving index...", 0, 1, nullptr);
        save_index_file(next);
        report(cb, ud, "Saving index...", 1, 1, nullptr);
    }

    entries = std::move(next);
    return dirty;
}

static lv_obj_t *menu = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *parent_obj = NULL;
static int target_focus_index = -1;
static int current_page = 0;
static std::vector<JournalEntry> journal_entries;
static bool cache_valid = false;

static ui_loading_t loader = {};
static uint32_t loader_started_ms = 0;

static void loader_show()
{
    ui_loading_open(&loader, "Loading Journal", "Starting...");
    loader_started_ms = lv_tick_get();
}

// Loader callback. Bridges the scan's (phase, cur/total, detail) triplet to
// the unified popup: the phase string becomes the detail line when there's
// no file name to show, and the progress bar tracks cur/total.
static void loader_update(void *ud, const char *phase, int cur, int total, const char *detail)
{
    (void)ud;
    if (!loader.overlay) return;

    // Compose a two-line detail: phase on top, current filename below.
    // Elapsed ms is appended to phase so debugging info stays visible
    // without adding another label. Long filenames are DOT-trimmed by
    // the shared detail label.
    char line[96];
    uint32_t elapsed = lv_tick_elaps(loader_started_ms);
    if (detail && detail[0]) {
        snprintf(line, sizeof(line), "%s (%lums)\n%s",
                 phase ? phase : "", (unsigned long)elapsed, detail);
    } else {
        snprintf(line, sizeof(line), "%s (%lums)",
                 phase ? phase : "", (unsigned long)elapsed);
    }
    ui_loading_set_progress(&loader, cur, total, line);
}

static void loader_hide()
{
    ui_loading_close(&loader);
}

void ui_journal_enter(lv_obj_t *parent);
void ui_journal_exit(lv_obj_t *parent);
static void journal_build_ui(lv_obj_t *parent);
static void schedule_bg_refresh();

static lv_obj_t *pending_journal_parent = NULL;
static void journal_unlock_result_cb(bool ok, void *ud)
{
    lv_obj_t *parent = (lv_obj_t *)ud;
    if (!ok) {
        pending_journal_parent = NULL;
        menu_show_deferred();
        return;
    }
    if (pending_journal_parent == parent) {
        pending_journal_parent = NULL;
        journal_build_ui(parent);
    }
}

static void free_post_user_data()
{
    if (!menu) return;
    lv_obj_t *main_page = lv_menu_get_cur_main_page(menu);
    if (!main_page) return;
    for (uint32_t i = 0; i < lv_obj_get_child_count(main_page); i++) {
        lv_obj_t *child = lv_obj_get_child(main_page, i);
        void *data = lv_obj_get_user_data(child);
        if (data) {
            lv_free(data);
            lv_obj_set_user_data(child, NULL);
        }
    }
}

static void teardown_menu_for_rerender()
{
    free_post_user_data();
    lv_obj_clean(menu);
    lv_obj_del(menu);
    menu = NULL;
    if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
}

// --- Background refresh -----------------------------------------------------
// Opening the journal renders the cached list immediately; when the filesystem
// has changed since the last scan we reconcile off the LVGL thread instead of
// blocking the app open on a directory walk of every note (seconds, with
// hundreds of synced notes). bg_refresh_timer holds the queued one-shot that
// hands the pass to the s_bg_job worker — the scan itself no longer runs inside
// the timer callback (that froze the UI and keyboard for seconds).
static lv_timer_t *bg_refresh_timer = nullptr;

struct BgRefreshResult {
    std::vector<JournalEntry> entries;
    uint32_t entries_gen = 0;  // s_entries_gen when the snapshot was taken
    uint32_t fs_gen = 0;       // hw_get_filesystem_generation() at snapshot time
    bool saved_index = false;  // the worker rewrote the index (bumps fs_gen once)
};
// Bumped whenever journal_entries is replaced or edited on the LVGL thread, so a
// worker result computed from an older snapshot is dropped instead of applied.
static uint32_t s_entries_gen = 0;
static void bg_refresh_done(BgRefreshResult &r);
static void start_first_run_refresh();
static void schedule_first_run_refresh();
// True while a first-run reconcile (P7.6) owns the journal loader.
static bool s_first_run_pending = false;
// The reconcile (directory walk + preview decrypts + index rewrite) runs here,
// off the LVGL thread. The worker only touches its own snapshot.
static core::OneShotJob<BgRefreshResult> s_bg_job(bg_refresh_done);

// Cheap FNV-1a over (filename, mtime) so a completed refresh can tell whether
// anything actually changed before tearing down and re-rendering the page —
// an unchanged journal must not flicker or steal focus.
static uint32_t entries_signature(const std::vector<JournalEntry> &v)
{
    uint32_t h = 2166136261u;
    for (const auto &e : v) {
        for (unsigned char c : e.filename) h = (h ^ c) * 16777619u;
        for (int s = 0; s < 32; s += 8) h = (h ^ ((e.mtime >> s) & 0xff)) * 16777619u;
    }
    h = (h ^ (uint32_t)v.size()) * 16777619u;
    return h;
}

// No-op progress sink whose sole purpose is to keep report()'s periodic
// watchdog yield alive during the background scan (report() only yields when a
// callback is supplied). Deliberately touches nothing on the UI.
static void bg_progress_cb(void *, const char *, int, int, const char *) {}

static void cancel_bg_refresh()
{
    if (bg_refresh_timer) {
        lv_timer_del(bg_refresh_timer);
        bg_refresh_timer = nullptr;
    }
}

static void bg_refresh_timer_cb(lv_timer_t *t)
{
    bg_refresh_timer = nullptr;
    lv_timer_del(t);
    if (menu == NULL || parent_obj == NULL) return;  // app closed before we ran
    // A worker is already running (or its result is waiting to drain): it lands
    // through bg_refresh_done, which reschedules if that result is stale.
    if (s_bg_job.busy()) return;
    BgRefreshResult *seed = new BgRefreshResult();
    seed->entries = journal_entries;  // copy: the worker owns it, the page keeps journal_entries
    seed->entries_gen = s_entries_gen;
    seed->fs_gen = hw_get_filesystem_generation();
    auto started = s_bg_job.start([seed]() {
        INPUT_PROF_SPAN("journal_bg");
        seed->saved_index = refresh_journal_entries(seed->entries, nullptr, nullptr);
        return seed;
    }, 10240, "journal_bg", 1, 0);
    if (started != core::OneShotJob<BgRefreshResult>::Start::Started) {
        // Task could not be created (Failed), or Attached to a stale worker
        // that busy() missed: run inline like before so the list still updates.
        seed->saved_index = refresh_journal_entries(seed->entries, bg_progress_cb, nullptr);
        bg_refresh_done(*seed);
        delete seed;
    }
}

// P7.6: the first-run reconcile hands its pass to s_bg_job instead of running
// it on the LVGL thread. If an earlier worker (detached by a previous exit) is
// still running, start() re-adopts it and its result lands here instead.
static void start_first_run_refresh()
{
    BgRefreshResult *seed = new BgRefreshResult();
    seed->entries_gen = s_entries_gen;   // journal_entries is empty here
    seed->fs_gen = hw_get_filesystem_generation();
    auto started = s_bg_job.start([seed]() {
        INPUT_PROF_SPAN("journal_bg");
        seed->saved_index = refresh_journal_entries(seed->entries, nullptr, nullptr);
        return seed;
    }, 10240, "journal_bg", 1, 0);
    if (started == core::OneShotJob<BgRefreshResult>::Start::Attached) {
        delete seed;   // the work lambda was not used; the running worker reports
    } else if (started == core::OneShotJob<BgRefreshResult>::Start::Failed) {
        // No task: run inline like before so the list still appears.
        seed->saved_index = refresh_journal_entries(seed->entries, loader_update, nullptr);
        bg_refresh_done(*seed);
        delete seed;
    }
}

static void first_run_timer_cb(lv_timer_t *t)
{
    bg_refresh_timer = nullptr;
    lv_timer_del(t);
    if (menu == NULL || parent_obj == NULL) {   // app closed before we ran
        s_first_run_pending = false;
        return;
    }
    start_first_run_refresh();
}

// Queue the first-run reconcile right after this frame; shares bg_refresh_timer
// so cancel_bg_refresh() (exit, rebuild) covers it.
static void schedule_first_run_refresh()
{
    cancel_bg_refresh();
    s_first_run_pending = true;
    ui_loading_set_indeterminate(&loader, "Scanning files...");
    bg_refresh_timer = lv_timer_create(first_run_timer_cb, 0, NULL);
    if (bg_refresh_timer) {
        lv_timer_set_repeat_count(bg_refresh_timer, 1);
    } else {
        start_first_run_refresh();
    }
}

static void bg_refresh_done(BgRefreshResult &r)
{
    // The first-run loader (P7.6) closes on any result, applied or dropped.
    bool first_run = s_first_run_pending;
    s_first_run_pending = false;
    if (first_run) loader_hide();
    // journal_entries changed (a delete) since the snapshot: drop this result.
    // cache_valid stays false, so the next open reconciles again; a first run
    // that is still on screen reconciles again now, without the loader.
    if (r.entries_gen != s_entries_gen) {
        if (first_run && menu != NULL && parent_obj != NULL) schedule_bg_refresh();
        return;
    }
    uint32_t before = entries_signature(journal_entries);
    journal_entries = std::move(r.entries);
    s_entries_gen++;
    // Any write since the snapshot may be missing from this scan. Our own index
    // rewrite does not count: hw_save_preferred_bytes never bumps the generation
    // for _idx.bin (P7.23 — expecting +1 here made every rewrite look stale).
    bool fresh = (hw_get_filesystem_generation() == r.fs_gen);
    cache_valid = fresh;
    // The index now reflects every write the dirty flag recorded; clear it so
    // the next open does not rescan for nothing (delete_confirm_cb does the same).
    if (fresh && r.saved_index) hw_set_filesystem_dirty(false);
    if (menu == NULL || parent_obj == NULL) return;  // closed: keep the warm cache
    if (entries_signature(journal_entries) != before) {
        teardown_menu_for_rerender();
        journal_build_ui(parent_obj);
    } else if (!fresh) {
        schedule_bg_refresh();
    }
}

// Queue the reconcile for shortly after this frame paints, so the cached list
// is already on screen (and interactive) before the scan runs.
static void schedule_bg_refresh()
{
    cancel_bg_refresh();
    bg_refresh_timer = lv_timer_create(bg_refresh_timer_cb, 120, NULL);
    if (bg_refresh_timer) lv_timer_set_repeat_count(bg_refresh_timer, 1);
}

static void do_exit()
{
    menu_show_deferred();  // onStop runs ui_journal_exit
}

static void next_page_cb(lv_event_t *e)
{
    current_page++;
    teardown_menu_for_rerender();
    ui_journal_enter(parent_obj);
}

static void prev_page_cb(lv_event_t *e)
{
    if (current_page > 0) {
        current_page--;
        teardown_menu_for_rerender();
        ui_journal_enter(parent_obj);
    }
}

static void exit_btn_cb(lv_event_t *e)
{
    do_exit();
}

static void post_focus_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    lv_obj_scroll_to_view(obj, LV_ANIM_ON);
}

static void post_defocus_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    lv_group_t *g = lv_group_get_default();
    if (!lv_group_get_editing(g)) return;
    lv_group_set_editing(g, false);
    lv_obj_t *scroll_area = lv_obj_get_child(obj, 0);
    if (scroll_area) {
        lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_scroll_to_y(scroll_area, 0, LV_ANIM_OFF);
    }
}

static void post_click_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    lv_group_t *g = lv_group_get_default();
    lv_obj_t *scroll_area = lv_obj_get_child(obj, 0);
    
    if (lv_group_get_editing(g) && lv_group_get_focused(g) == obj) {
        lv_group_set_editing(g, false);
        if (scroll_area) {
            lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_scroll_to_y(scroll_area, 0, LV_ANIM_ON);
        }
    } else {
        lv_group_set_editing(g, true);
        if (scroll_area) lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void delete_confirm_cb(bool yes, void *ud)
{
    const char *path = (const char *)ud;
    bool deleted = false;

    if (yes) {
        if (hw_delete_file(path)) {
            journal_entries.erase(std::remove_if(journal_entries.begin(), journal_entries.end(),
                                              [&](const JournalEntry &be) { return be.filename == path; }),
                               journal_entries.end());
            s_entries_gen++;
            save_index_file(journal_entries);
            cache_valid = true;
            // A running worker may still rewrite the index from its older
            // snapshot after this save; leave the flag set so the next open rescans.
            if (!s_bg_job.busy()) hw_set_filesystem_dirty(false);
            deleted = true;
        }
    } else {
        target_focus_index = -1;
    }

    if (deleted) {
        teardown_menu_for_rerender();
        ui_journal_enter(parent_obj);
    }

    if (path) lv_free((void *)path);
}

static void show_delete_confirm(const char *filename)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "Delete this entry?\n%s", filename);

    char *path_dup = (char *)lv_malloc(strlen(filename) + 1);
    strcpy(path_dup, filename);

    ui_confirm("Confirm", msg, delete_confirm_cb, path_dup);
}

static void journal_key_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_KEY) return;
    uint32_t key = lv_event_get_key(e);
    if (key == 'r' || key == 'R') {
        cache_valid = false;
        lv_obj_t *parent = parent_obj;  // ui_journal_exit nulls it
        ui_journal_exit(NULL);
        ui_journal_enter(parent);
        return;
    }
    if (key != 'd' && key != 'D' && key != LV_KEY_BACKSPACE) return;

    lv_group_t *g = lv_group_get_default();
    lv_obj_t *focused = lv_group_get_focused(g);
    if (!focused) return;
    const char *filename = (const char *)lv_obj_get_user_data(focused);
    if (!filename) return;

    lv_obj_t *page = lv_menu_get_cur_main_page(menu);
    if (page) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(page); i++) {
            if (lv_obj_get_child(page, i) == focused) {
                target_focus_index = (int)i;
                break;
            }
        }
    }
    show_delete_confirm(filename);
}

static std::string parse_filename_to_human(const std::string &filename)
{
    /* Listing returns names like "notes/20260418_120530.txt" — skip past any
     * leading slash and the "notes/" prefix before parsing the timestamp. */
    size_t start = (filename[0] == '/') ? 1 : 0;
    if (filename.compare(start, 6, "notes/") == 0) start += 6;
    if (filename.length() < start + 15) return filename;

    std::string y = filename.substr(start, 4);
    std::string m = filename.substr(start + 4, 2);
    std::string d = filename.substr(start + 6, 2);
    std::string hh = filename.substr(start + 9, 2);
    std::string mm = filename.substr(start + 11, 2);
    std::string ss = filename.substr(start + 13, 2);

    const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    int month_idx = atoi(m.c_str()) - 1;
    if (month_idx < 0 || month_idx > 11) return filename;

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%s %s, %s - %s:%s:%s",
             months[month_idx], d.c_str(), y.c_str(), hh.c_str(), mm.c_str(), ss.c_str());
    return std::string(buffer);
}

static void post_scroll_indicator_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *indicator = (lv_obj_t *)lv_event_get_user_data(e);
    if (!indicator) return;
    
    if (lv_obj_get_scroll_bottom(obj) <= 0) {
        lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_journal_enter(lv_obj_t *parent)
{
    if (notes_crypto_is_enabled() && !notes_crypto_is_unlocked()) {
        pending_journal_parent = parent;
        ui_passphrase_unlock(journal_unlock_result_cb, parent);
        return;
    }
    journal_build_ui(parent);
}

static void journal_build_ui(lv_obj_t *parent)
{
    if (menu != NULL) return;
    enable_keyboard();
    parent_obj = parent;

    // This build supersedes any refresh queued by a previous one.
    cancel_bg_refresh();
    // A first-run reconcile whose worker is gone no longer owns the loader.
    if (!s_bg_job.busy()) s_first_run_pending = false;

    bool was_dirty = hw_get_filesystem_dirty();
    if (was_dirty) hw_set_filesystem_dirty(false);

    // Always get *something* on screen instantly: reuse the in-memory list, or
    // fall back to the persisted on-disk index. When nothing has changed since
    // the last scan (dirty flag clear) the index is authoritative and we skip
    // the rescan entirely — the original cold-boot fast path.
    if (journal_entries.empty()) {
        std::vector<JournalEntry> snapshot;
        if (load_index_file(snapshot) && !snapshot.empty()) {
            journal_entries = std::move(snapshot);
            s_entries_gen++;
            if (!was_dirty) cache_valid = true;
        }
    }
    if (was_dirty) cache_valid = false;

    bool have_cache = !journal_entries.empty();
    bool need_refresh = !cache_valid;
    // Only block behind the loading popup when there is genuinely nothing to
    // display yet (true first run). Otherwise render the cache now and reconcile
    // in the background so the app opens instantly even with a stale index.
    bool show_loader = need_refresh && !have_cache;
    if (show_loader) loader_show();
    menu = create_menu(parent, nullptr);
    // P7.6: the first-run reconcile (index missing, empty or undecryptable:
    // one snippet decrypt per note plus an encrypted index save) runs on
    // s_bg_job like the IS.9 refresh, not inside lv_timer_handler. The loader
    // stays up (indeterminate: the worker can't touch LVGL) over the empty page
    // until bg_refresh_done applies the result and hides it.
    // The job is started from a one-shot timer once this build returns, so an
    // inline completion (emulator, or no task) can't rebuild the page under us.
    if (show_loader) need_refresh = false;

    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_style_pad_all(main_page, 0, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(main_page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(main_page, journal_key_event_cb, LV_EVENT_KEY, NULL);

    // Back button moves to the top status bar. The journal-specific focus and
    // key handlers stay attached to it.
    lv_obj_t *exit_btn = ui_show_back_button(exit_btn_cb);
    if (exit_btn) {
        lv_obj_add_event_cb(exit_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(exit_btn, journal_key_event_cb, LV_EVENT_KEY, NULL);
    }

    int total_files = (int)journal_entries.size();
    int start_idx = current_page * JOURNAL_PAGE_SIZE;
    int end_idx = std::min(start_idx + JOURNAL_PAGE_SIZE, total_files);

    if (start_idx >= total_files && total_files > 0) {
        current_page = (total_files - 1) / JOURNAL_PAGE_SIZE;
        start_idx = current_page * JOURNAL_PAGE_SIZE;
        end_idx = total_files;
    }

    lv_obj_t *focus_target = exit_btn;
    // The back button no longer occupies main_page's child 0, so child indices
    // for prev_btn_cont and post_conts start at 0 instead of 1.
    int current_child_idx = -1;
    std::vector<std::pair<lv_obj_t *, lv_obj_t *>> trunc_checks;

    if (total_files == 0) {
        lv_obj_t *empty_label = lv_label_create(main_page);
        lv_label_set_text(empty_label, "No entries found.");
        lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty_label, lv_pct(100));
    } else {
        const lv_font_t *font = get_journal_font();

        if (current_page > 0) {
            lv_obj_t *prev_cont = lv_menu_cont_create(main_page);
            lv_obj_t *prev_btn = lv_btn_create(prev_cont);
            lv_obj_t *prev_label = lv_label_create(prev_btn);
            lv_label_set_text(prev_label, LV_SYMBOL_UP " Previous Page");
            lv_obj_add_event_cb(prev_btn, prev_page_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), prev_btn);
            lv_obj_add_event_cb(prev_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
            current_child_idx++;
        }

        int render_total = end_idx - start_idx;
        trunc_checks.reserve(render_total);
        for (int i = start_idx; i < end_idx; ++i) {
            const JournalEntry &entry = journal_entries[i];
            current_child_idx++;
            loader_update(nullptr, "Rendering entries...",
                          i - start_idx, render_total, entry.filename.c_str());

            lv_obj_t *post_cont = lv_obj_create(main_page);
            lv_obj_set_width(post_cont, lv_pct(100));
            lv_obj_set_height(post_cont, LV_SIZE_CONTENT);
            lv_obj_set_style_max_height(post_cont, 130, 0);
            lv_obj_set_flex_flow(post_cont, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(post_cont, 0, 0); 
            lv_obj_set_style_pad_gap(post_cont, 0, 0);
            lv_obj_set_style_border_width(post_cont, 0, 0); 
            lv_obj_set_style_bg_opa(post_cont, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(post_cont, UI_COLOR_SURFACE, 0);
            lv_obj_set_style_clip_corner(post_cont, true, 0);
            lv_obj_clear_flag(post_cont, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *scroll_area = lv_obj_create(post_cont);
            lv_obj_set_width(scroll_area, lv_pct(100));
            lv_obj_set_height(scroll_area, LV_SIZE_CONTENT);
            lv_obj_set_style_max_height(scroll_area, 125, 0);
            lv_obj_set_flex_flow(scroll_area, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(scroll_area, 0, 0);
            lv_obj_set_style_pad_gap(scroll_area, 0, 0);
            lv_obj_set_style_border_width(scroll_area, 0, 0);
            lv_obj_set_style_bg_opa(scroll_area, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE); 
            lv_obj_clear_flag(scroll_area, LV_OBJ_FLAG_SCROLL_ELASTIC); 
            lv_obj_set_scrollbar_mode(scroll_area, LV_SCROLLBAR_MODE_AUTO);

            char *fn_dup = (char *)lv_malloc(entry.filename.length() + 1);
            strcpy(fn_dup, entry.filename.c_str());
            lv_obj_set_user_data(post_cont, fn_dup);

            lv_obj_set_style_border_width(post_cont, UI_BORDER_W, LV_STATE_FOCUSED);
            lv_obj_set_style_border_color(post_cont, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
            lv_obj_set_style_radius(post_cont, UI_RADIUS, 0);

            lv_group_add_obj(lv_group_get_default(), post_cont);
            lv_obj_add_event_cb(post_cont, post_focus_cb, LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(post_cont, post_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
            lv_obj_add_event_cb(post_cont, post_click_cb, LV_EVENT_CLICKED, NULL);
            
            // Add internal scroll handler for editing mode
            lv_obj_add_event_cb(post_cont, [](lv_event_t *ev) {
                lv_obj_t *wrapper = (lv_obj_t *)lv_event_get_current_target(ev);
                lv_obj_t *scroller = lv_obj_get_child(wrapper, 0);
                lv_group_t *g = lv_group_get_default();
                if (!lv_group_get_editing(g) || lv_group_get_focused(g) != wrapper) return;

                ui_scroll_key_clamped(scroller, lv_event_get_key(ev), 40, LV_ANIM_ON);
            }, LV_EVENT_KEY, NULL);

            lv_obj_add_event_cb(post_cont, journal_key_event_cb, LV_EVENT_KEY, NULL);

            if (target_focus_index != -1) {
                if (current_child_idx <= target_focus_index) {
                    focus_target = post_cont;
                }
            }

            lv_obj_t *header = lv_label_create(scroll_area);
            lv_label_set_text(header, parse_filename_to_human(entry.filename).c_str());
            lv_obj_set_style_text_font(header, font, 0);
            lv_obj_set_style_text_color(header, UI_COLOR_MUTED, 0);
            lv_obj_set_width(header, lv_pct(100));
            lv_obj_set_style_pad_left(header, 4, 0); 

            lv_obj_t *line = lv_obj_create(scroll_area);
            lv_obj_set_size(line, lv_pct(100), 1);
            lv_obj_set_style_bg_color(line, lv_color_mix(UI_COLOR_FG, UI_COLOR_BG, LV_OPA_30), 0);
            lv_obj_set_style_border_width(line, 0, 0);
            lv_obj_set_style_pad_all(line, 0, 0); 

            lv_obj_t *label = lv_label_create(scroll_area);
            lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
            /* set_text copies into the label's own buffer; feed it the cached
             * snippet directly rather than taking a second heap copy per entry. */
            lv_label_set_text(label, entry.snippet.c_str());
            lv_obj_set_width(label, lv_pct(100));
            lv_obj_set_style_text_font(label, font, 0);
            lv_obj_set_style_text_color(label, UI_COLOR_FG, 0);
            lv_obj_set_style_pad_left(label, 4, 0); 
            lv_obj_set_style_pad_right(label, 4, 0);

            lv_obj_t *trunc_icon = lv_label_create(post_cont);
            lv_label_set_text(trunc_icon, LV_SYMBOL_DOWN);
            lv_obj_set_style_text_font(trunc_icon, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(trunc_icon, UI_COLOR_ACCENT, 0);
            lv_obj_add_flag(trunc_icon, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_align(trunc_icon, LV_ALIGN_BOTTOM_RIGHT, -12, -5);
            lv_obj_set_style_bg_opa(trunc_icon, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(trunc_icon, LV_OBJ_FLAG_HIDDEN);

            lv_obj_add_event_cb(scroll_area, post_scroll_indicator_cb, LV_EVENT_SCROLL, trunc_icon);
            trunc_checks.emplace_back(scroll_area, trunc_icon);
        }

        lv_obj_t *footer_cont = lv_obj_create(main_page);
        lv_obj_set_width(footer_cont, lv_pct(100));
        lv_obj_set_height(footer_cont, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(footer_cont, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(footer_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(footer_cont, 5, 0);
        lv_obj_set_style_bg_opa(footer_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(footer_cont, 0, 0);

        if (end_idx < total_files) {
            lv_obj_t *next_btn = lv_btn_create(footer_cont);
            lv_obj_t *next_label = lv_label_create(next_btn);
            lv_label_set_text(next_label, LV_SYMBOL_DOWN " Next");
            lv_obj_add_event_cb(next_btn, next_page_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(lv_group_get_default(), next_btn);
            lv_obj_add_event_cb(next_btn, post_focus_cb, LV_EVENT_FOCUSED, NULL);
        }

        lv_obj_t *page_info = lv_label_create(footer_cont);
        char buf[32];
        snprintf(buf, sizeof(buf), "Page %d/%d", current_page + 1, (total_files + JOURNAL_PAGE_SIZE - 1) / JOURNAL_PAGE_SIZE);
        lv_label_set_text(page_info, buf);
        lv_obj_set_style_text_font(page_info, font, 0);
        lv_obj_set_style_text_color(page_info, UI_COLOR_MUTED, 0);
        lv_obj_set_flex_grow(page_info, 1);
        lv_obj_set_style_text_align(page_info, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_pad_right(page_info, 10, 0);
    }

    lv_menu_set_page(menu, main_page);

    lv_obj_update_layout(main_page);
    for (auto &pair : trunc_checks) {
        lv_obj_t *scroll_area = pair.first;
        lv_obj_t *trunc_icon = pair.second;
        if (lv_obj_get_scroll_bottom(scroll_area) > 0) {
            lv_obj_remove_flag(trunc_icon, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(scroll_area, LV_OBJ_FLAG_SCROLLABLE);
    }

    if (focus_target) {
        lv_group_focus_obj(focus_target);
        lv_obj_scroll_to_view(focus_target, LV_ANIM_OFF);
    }

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button([](lv_event_t *e) {
        do_exit();
    }, NULL);
#endif

    // Cached list is now on screen; reconcile with the filesystem in the
    // background if it may be stale.
    if (need_refresh) schedule_bg_refresh();
    if (show_loader) schedule_first_run_refresh();

    // A first-run reconcile still owns the loader; bg_refresh_done hides it.
    if (!s_first_run_pending) loader_hide();
}

void ui_journal_exit(lv_obj_t *parent)
{
    cancel_bg_refresh();
    s_bg_job.teardown();  // detach a running worker; it frees its own result
    s_first_run_pending = false;
    loader_hide();
    ui_hide_back_button();
    disable_keyboard();
    if (menu) {
        teardown_menu_for_rerender();
    } else if (quit_btn) {
        lv_obj_del_async(quit_btn);
        quit_btn = NULL;
    }
    target_focus_index = -1;
    current_page = 0;
    parent_obj = NULL;
    pending_journal_parent = NULL;  // a still-open unlock prompt must not build
}

} // namespace

namespace apps {
class JournalApp : public core::App {
public:
    JournalApp() : core::App("Journal") {}
    void onStart(lv_obj_t *parent) override { ui_journal_enter(parent); }
    void onStop() override {
        ui_journal_exit(getRoot());
        core::App::onStop();
    }
};

APP_FACTORY(make_journal_app, JournalApp)
} // namespace apps
