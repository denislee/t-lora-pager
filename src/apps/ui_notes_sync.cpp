/**
 * @file      ui_notes_sync.cpp
 * @brief     GitHub notes sync: the phone first (PagerLink notes.sync), the
 *            lilyhub as a hardware-only fallback — internal-only source.
 *
 * Route, picked per run in notes_sync_worker (HUB_REMOVAL_PLAN.md H2.5):
 *   1. Phone connected over PagerLink → notes_link_sync() (hal/notes_link.h)
 *      with the internal /notes manifest (vol "int", targets ["github"]).
 *      The phone holds repo/branch/PAT (D-H2) and pulls the raw bytes with
 *      fs.read, so this path needs no GitHub config, no Wi-Fi and no unlock.
 *      A move-capable phone then deletes each note it verified on GitHub
 *      with a conditional fs.delete (size + crc32, cap "fs.delete.if"), so
 *      the notes leave the pager; the totals carry deleted/kept.
 *      Works in the emulator.
 *   2. Hardware only, no phone, hub enabled → the hub flow below, unchanged
 *      until the hub is deleted (H4.2, gated on BLE hardware: D-H1):
 *        a. Mirror every internal-FFat note to the lilyhub via
 *           POST /api/notes/upload, so the hub has the canonical copy.
 *        b. POST /api/notes/sync with the same manifest; the hub does the
 *           additive GitHub push (list + PUT) on a real CPU.
 *   3. Neither → "Connect your phone (or set Settings » Local Hub)".
 *
 * The SD card is no longer consulted on sync. The user copies
 * SD-resident notes to the hub explicitly via the Storage settings
 * action when needed; the prune-on-overflow path also mirrors to the
 * hub. This makes the hub authoritative for sync and removes the
 * "FFat-wins-over-SD" merge that used to live here.
 *
 * The push is strictly additive: files already on the remote are
 * skipped (never updated, never deleted). On-device edits to a name
 * already pushed will not propagate by themselves — by design, this
 * is a backup-only flow, not a mirror. To re-push a changed note,
 * delete or rename it on the remote first.
 *
 * Bytes go to the repo as raw ciphertext when notes crypto is enabled;
 * hw_read_internal_bytes_raw() bypasses the decrypt-on-read path so
 * the repo gets the opaque Salted__ blob.
 *
 * State reset on onStop (exit_cb):
 *   widgets: s_log_label, s_log_scroll               — nulled
 *   text:    s_log_text                              — cleared
 *   timers:  s_bg_timer (log drain ticker)           — lv_timer_del + null
 *   queue:   s_pending_log                           — cleared (s_log_mu)
 *   job:     s_sync_job (sync worker,                — teardown(): left to
 *            core::OneShotJob)                         finish (it only
 *                                                      touches the log
 *                                                      queue); its
 *                                                      completion is dropped
 *                                                      unless a re-entry
 *                                                      re-attaches it
 *   flags:   s_syncing                               — cleared
 * If you add a new cached LVGL pointer, timer, or worker, list it above AND
 * extend exit_cb() — a sync worker outliving the UI would write into freed
 * log buffers.
 */
#include "../ui_define.h"
#include "../hal/storage.h"
#include "../hal/system.h"
#include "../hal/wireless.h"
#include "../hal/notes_crypto.h"
#include "../hal/secrets.h"
#include "../hal/hub.h"
#include "../hal/link.h"
#include "../hal/notes_link.h"
#include "../hal/str_encode.h"
#include "../hal/nvs.h"
#include "../core/app.h"
#include "../core/app_manager.h"
#include "../core/one_shot_job.h"
#include "app_registry.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#ifdef ARDUINO
extern "C" {
#include "cJSON.h"
}
#endif

namespace {

#define NSYNC_PREFS_NS      "notesync"
#define NSYNC_DEFAULT_BRANCH "main"

// --- NVS helpers ----------------------------------------------------------

// Thin namespace-binding wrappers over the shared hal::nvs_* helpers; the
// begin/get/put/end boilerplate (and its emulator stub) lives in hal/nvs.cpp.
static std::string load_pref(const char *key, const char *dflt = "") { return hal::nvs_get_str(NSYNC_PREFS_NS, key, dflt); }
static void        save_pref(const char *key, const char *value)     { hal::nvs_set_str(NSYNC_PREFS_NS, key, value); }

// GitHub PAT persistence delegates to hal/secrets — same encrypted-NVS
// wrapper the Telegram bearer uses, sharing the notes passphrase. A PAT
// with contents:write on a private repo is a write credential, so we
// never want it at rest plaintext; the legacy `token` slot is wiped at
// boot (apps::register_all) to close that door for any pre-hardening installs.
static bool token_is_encrypted() {
    return hal::secret_exists(NSYNC_PREFS_NS, "token_enc");
}

// secret_store trims the pasted whitespace that would otherwise yield a
// silent 401 from GitHub.
static bool save_token(const char *value, std::string *err) {
    return hal::secret_store(NSYNC_PREFS_NS, "token_enc", value, err);
}

// --- state ----------------------------------------------------------------

static lv_obj_t *s_log_label = nullptr;
static lv_obj_t *s_log_scroll = nullptr;
static std::string s_log_text;
static bool s_syncing = false;

// The sync runs on a core::OneShotJob worker (s_sync_job, below) so the LVGL
// thread keeps servicing the keyboard/encoder. The worker only ever touches
// `s_pending_log` (under s_log_mu); a UI-thread timer drains the queue, and
// the job's completion drains it one last time and finalizes.
static std::mutex s_log_mu;
static lv_timer_t *s_bg_timer = nullptr;
static std::vector<std::string> s_pending_log; // bg → ui log queue (s_log_mu)

static void bg_log_append(const char *line)
{
    std::lock_guard<std::mutex> lock(s_log_mu);
    s_pending_log.emplace_back(line ? line : "");
}

// Drop queued lines (no UI to render them / a fresh run).
static void bg_log_clear()
{
    std::lock_guard<std::mutex> lock(s_log_mu);
    s_pending_log.clear();
}

static void bg_log_appendf(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bg_log_append(buf);
}

// P7.18: trim only past kLogHigh, down to at most kLogLow at a line
// boundary, so the full label re-set happens once per ~2 KB of new text.
static constexpr size_t kLogHigh = 8000;
static constexpr size_t kLogLow  = 6000;

// LV_EVENT_SIZE_CHANGED on s_log_label: scroll to the newest line after the
// normal layout pass instead of forcing lv_obj_update_layout per append.
static void log_size_changed_cb(lv_event_t *e)
{
    (void)e;
    if (s_log_scroll) lv_obj_scroll_to_y(s_log_scroll, LV_COORD_MAX, LV_ANIM_OFF);
}

static void log_append(const char *line)
{
    std::string add = line ? line : "";
    add.push_back('\n');
    s_log_text.append(add);

    // Bound the transcript so a long sync run can't grow this string (and
    // the label it backs) without limit. Drop whole lines from the front
    // once over cap — mirrors ui_chat.cpp's log_append.
    bool trimmed = false;
    if (s_log_text.size() > kLogHigh) {
        size_t cut = s_log_text.size() - kLogLow;
        size_t nl = s_log_text.find('\n', cut);
        s_log_text.erase(0, nl == std::string::npos ? cut : nl + 1);
        trimmed = true;
    }

    if (s_log_label) {
        if (trimmed) lv_label_set_text(s_log_label, s_log_text.c_str());
        else lv_label_ins_text(s_log_label, LV_LABEL_POS_LAST, add.c_str());
    }
}

// --- config ---------------------------------------------------------------

struct Config {
    std::string repo;        // "owner/repo"
    std::string branch;      // default "main"
    std::string token;       // GitHub PAT (contents:write)
};

#ifdef ARDUINO

static std::string load_token_plain() {
    return hal::secret_load(NSYNC_PREFS_NS, "token_enc");
}

static bool load_config(Config &c, std::string *err)
{
    c.repo     = load_pref("repo");
    c.branch   = load_pref("branch", NSYNC_DEFAULT_BRANCH);
    c.token    = load_token_plain();
    if (c.branch.empty()) c.branch = NSYNC_DEFAULT_BRANCH;
    if (c.repo.empty()) {
        if (err) *err = "Repo not set (Settings » Notes Sync).";
        return false;
    }
    if (c.repo.find('/') == std::string::npos) {
        if (err) *err = "Repo must be owner/name.";
        return false;
    }
    if (c.token.empty()) {
        if (err) *err = token_is_encrypted()
                        ? "Token locked — open Notes to unlock."
                        : "GitHub token not set.";
        return false;
    }
    return true;
}

// --- Hub API ---------------------------------------------------------------

// Hub-delegated sync: the device builds a manifest of every local note (name
// + base64-of-raw-bytes) and POSTs it to the configured lilyhub. The hub
// performs the GitHub list+PUT round-trips on a real CPU with HTTP keepalive
// and bounded parallelism, returning a JSON summary the device parses back
// into log lines. On any transport failure or non-2xx response we return
// false and the caller logs the error.
//
// Token is sent in the request body to the hub. It's an in-LAN call so plain
// HTTP is acceptable for this iteration; a future hub-side stored token can
// remove the cleartext-on-LAN exposure.
struct LocalNote {
    std::string name;   // internal-FFat path without the leading '/'
};

static bool try_hub_sync(const Config &cfg,
                         const std::vector<LocalNote> &local,
                         std::string *err)
{
    std::string hub = hal::hub_get_url();
    if (hub.empty()) {
        if (err) *err = "hub disabled";
        return false;
    }

    // Build the JSON manifest. We base64-encode raw on-disk bytes so encrypted
    // notes (Salted__ blobs) pass through verbatim — the hub never sees and
    // never needs the user's notes passphrase. Body size is dominated by the
    // base64 output (~133% of the source); the ESP32 heap is fine with the
    // ~30 small text notes typical of this app, but a much larger corpus
    // would warrant chunking.
    std::string body;
    body.reserve(256 + 1024 * local.size());
    body += "{\"repo\":\"";   body += hal::json_escape(cfg.repo);   body += "\",";
    body += "\"branch\":\""; body += hal::json_escape(cfg.branch); body += "\",";
    body += "\"token\":\"";  body += hal::json_escape(cfg.token);  body += "\",";
    body += "\"files\":[";
    bool first = true;
    int read_skipped = 0;
    for (const auto &n : local) {
        std::vector<uint8_t> bytes;
        std::string abs = "/" + n.name;
        if (!hw_read_internal_bytes_raw(abs.c_str(), bytes)) { read_skipped++; continue; }
        std::string b64;
        if (!hal::base64_encode(bytes.data(), bytes.size(), b64)) { read_skipped++; continue; }
        if (!first) body.push_back(',');
        first = false;
        body += "{\"name\":\"";        body += hal::json_escape(n.name); body += "\",";
        body += "\"content_b64\":\""; body += b64;                  body += "\"}";
    }
    body += "]}";

    std::string url = hub + "/api/notes/sync";
    std::string resp;
    int code = 0;
    std::string terr;
    bool ok = hw_http_request(url.c_str(), "POST",
                              body.c_str(), body.size(),
                              "application/json",
                              nullptr, resp, &code, &terr);
    hal::secret_scrub(body);
    if (!ok || code / 100 != 2) {
        if (err) {
            if (!terr.empty()) *err = terr;
            else if (code != 0) {
                char buf[24];
                snprintf(buf, sizeof(buf), "HTTP %d", code);
                *err = buf;
                if (!resp.empty()) *err += ": " + resp.substr(0, 120);
            } else {
                *err = "no response";
            }
        }
        return false;
    }

    // Parse the hub's summary. Shape: {"uploaded":N,"already":M,
    // "errors":[{"name":"...","error":"..."}, ...]}
    int uploaded = 0, already = 0;
    std::vector<std::pair<std::string,std::string>> errors;
    cJSON *j = cJSON_Parse(resp.c_str());
    if (!j) {
        if (err) *err = "bad hub json";
        return false;
    }
    cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "uploaded");
    cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "already");
    cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "errors");
    if (cJSON_IsNumber(u)) uploaded = u->valueint;
    if (cJSON_IsNumber(a)) already = a->valueint;
    if (cJSON_IsArray(e)) {
        int n = cJSON_GetArraySize(e);
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(e, i);
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *er = cJSON_GetObjectItemCaseSensitive(it, "error");
            errors.emplace_back(
                cJSON_IsString(nm) ? nm->valuestring : "?",
                cJSON_IsString(er) ? er->valuestring : "?");
        }
    }
    cJSON_Delete(j);

    for (const auto &p : errors) {
        bg_log_appendf("  up %s FAIL: %s", p.first.c_str(), p.second.c_str());
    }
    bg_log_appendf("Notes: +%d =%d (fail %d, skip %d)",
                uploaded, already, (int)errors.size(), read_skipped);
    return true;
}

// --- sync driver ----------------------------------------------------------

// Runs on the sync worker. Touches only the bg log queue; never the LVGL
// widget tree.
static void run_sync_bg()
{
    std::string err;
    Config cfg;
    if (!load_config(cfg, &err)) {
        bg_log_appendf("ERR: %s", err.c_str());
        hal::secret_scrub(cfg.token);  // loaded even when repo validation fails
        return;
    }
    if (!hw_get_wifi_connected()) {
        bg_log_append("ERR: WiFi not connected.");
        hal::secret_scrub(cfg.token);
        return;
    }

    if (!hal::hub_is_enabled()) {
        bg_log_append("ERR: Local Hub is not enabled.");
        bg_log_append("     Set the URL in Settings " "\xC2\xBB" " Local Hub.");
        hal::secret_scrub(cfg.token);
        return;
    }

    bg_log_appendf("Repo: %s  Branch: %s", cfg.repo.c_str(), cfg.branch.c_str());

    // 1. Build the local candidate list from internal FFat only. The SD card
    //    is intentionally not consulted here — Settings » Storage » "Copy
    //    Notes -> Hub" is the explicit path for SD-resident notes.
    auto strip_slash = [](std::string &p) {
        if (!p.empty() && p[0] == '/') p.erase(0, 1);
    };

    std::vector<std::string> ffat_files;
    hw_get_internal_txt_files(ffat_files);
    std::vector<LocalNote> local;
    local.reserve(ffat_files.size());
    for (auto &p : ffat_files) {
        strip_slash(p);
        if (!p.empty()) local.push_back({p});
    }
    bg_log_appendf("Local notes: %u (internal only)", (unsigned)local.size());

    // 2. Mirror every internal note to the hub before the GitHub push so the
    //    hub keeps a copy regardless of how the GitHub side resolves. Read
    //    raw bytes so encrypted Salted__ blobs ride through verbatim.
    int hub_up_ok = 0, hub_up_fail = 0;
    for (const auto &n : local) {
        std::vector<uint8_t> bytes;
        std::string abs = "/" + n.name;
        if (!hw_read_internal_bytes_raw(abs.c_str(), bytes)) {
            bg_log_appendf("  read %s: skip", n.name.c_str());
            continue;
        }
        HalError uerr = hal::hub_upload_note(n.name.c_str(), bytes.data(), bytes.size());
        if (uerr == HalError::Ok) {
            hub_up_ok++;
        } else {
            hub_up_fail++;
            bg_log_appendf("  hub %s FAIL: %s", n.name.c_str(), hal_error_string(uerr));
        }
    }
    bg_log_appendf("Hub upload: ok=%d fail=%d", hub_up_ok, hub_up_fail);

    // 3. Have the hub run the additive GitHub push. We still hand it the
    //    file manifest in-band so a single round-trip covers the whole sync.
    bg_log_append("[hub] Uploads via Local Hub (notes stay here)");
    std::string herr;
    if (!try_hub_sync(cfg, local, &herr)) {
        bg_log_appendf("[hub] failed: %s", herr.c_str());
    }

    hal::secret_scrub(cfg.token);
}

#endif  // ARDUINO

// Phone path: the internal /notes manifest goes to notes.sync with target
// "github"; the phone pulls the raw bytes and pushes them. No GitHub config,
// Wi-Fi or unlock needed here (D-H2): encrypted notes travel as ciphertext.
static void run_phone_sync()
{
    bg_log_append("[phone] Moves notes to GitHub via your phone");
    std::vector<NoteEntry> files;
    std::string err;
    if (!notes_link_manifest(false, files, err)) {
        bg_log_appendf("ERR: %s", err.c_str());
        return;
    }
    bg_log_appendf("Local notes: %u (internal only)", (unsigned)files.size());
    HalResult<NotesSyncSummary> r = notes_link_sync(
        files, {"github"}, [](const std::string &l) { bg_log_appendf("  %s", l.c_str()); }, err);
    if (!r) {
        bg_log_appendf("[phone] failed: %s", err.empty() ? hal_error_string(r.error()) : err.c_str());
        return;
    }
    bg_log_appendf("[phone] uploaded=%d already=%d deleted=%d kept=%d errors=%u",
                   r.value().uploaded, r.value().already, r.value().deleted, r.value().kept,
                   (unsigned)r.value().errors.size());
}

// Worker body: phone first, then (hardware only) the hub, else an error.
static void notes_sync_worker()
{
    if (hw_link_connected()) {
        run_phone_sync();
#ifdef ARDUINO
    } else if (hal::hub_is_enabled()) {
        if (token_is_encrypted() && !notes_crypto_is_unlocked()) {
            bg_log_append("ERR: Notes session locked. Open Notes and unlock.");
        } else {
            run_sync_bg();
        }
#endif
    } else {
        bg_log_append("ERR: Connect your phone (or set Settings " "\xC2\xBB" " Local Hub)");
    }
    bg_log_append("Done.");
}

// --- UI -------------------------------------------------------------------

static void back_btn_cb(lv_event_t *) { menu_show_deferred(); }

// LVGL timer (and the job's completion): drain queued log lines from the
// worker into the log label.
static void notes_sync_drain_tick(lv_timer_t *t)
{
    (void)t;
    std::vector<std::string> drained;
    {
        std::lock_guard<std::mutex> lock(s_log_mu);
        drained.swap(s_pending_log);
    }
    if (!drained.empty()) {
        if (s_log_label) {
            // One label update (+ scroll) per tick regardless of how many
            // lines the bg task queued, instead of one per line.
            std::string batch;
            for (size_t i = 0; i < drained.size(); ++i) {
                if (i) batch.push_back('\n');
                batch += drained[i];
            }
            log_append(batch.c_str());
        }
    }
}

struct SyncDone {};

// Job delivery (LVGL thread): the worker has queued its last line ("Done.");
// show it and finalize.
static void sync_done(SyncDone &)
{
    notes_sync_drain_tick(nullptr);
    s_syncing = false;
    if (s_bg_timer) {
        lv_timer_del(s_bg_timer);
        s_bg_timer = nullptr;
    }
}

static core::OneShotJob<SyncDone> s_sync_job(sync_done);

static void start_sync()
{
    if (s_syncing) return;
    s_syncing = true;
    hw_feedback();
    s_log_text.clear();
    if (s_log_label) lv_label_set_text(s_log_label, "");
    log_append("Starting sync...");
    // Drop any leftover queue entries from a previous (cancelled-by-exit) run.
    bg_log_clear();

    // A previous run still finishing in the background (the app was left and
    // re-entered mid-sync) is re-attached rather than duplicated: its
    // completion then finalizes this run (DC2.0.7e).
    // 8 KB stack: HTTPS round-trip via mbedtls is the floor here — TLS
    // handshake plus cert chain easily eats 6 KB on its own.
    auto started = s_sync_job.start([]() {
        notes_sync_worker();
        return new SyncDone();
    }, 8192, "nsync_bg", 2);
    if (started == core::OneShotJob<SyncDone>::Start::Failed) {
        log_append("ERR: failed to start sync task");
        s_syncing = false;
        return;
    }
    if (!s_bg_timer) {
        s_bg_timer = lv_timer_create(notes_sync_drain_tick, 100, nullptr);
    }
}

static void enter(lv_obj_t *parent)
{
    s_log_text.clear();
    ui_show_back_button(back_btn_cb);

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Status banner: one-line summary of what the sync will do.
    lv_obj_t *status = lv_label_create(parent);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status, lv_pct(100));
    lv_obj_set_style_text_color(status, UI_COLOR_MUTED, 0);
    std::string repo = load_pref("repo");
    if (repo.empty()) {
        lv_label_set_text(status,
            "Not configured.\n"
            "Set the repo and token in Settings " "\xC2\xBB" " Notes Sync.\n"
            "Private repos work — create a fine-grained PAT with "
            "Contents: read/write on this repo only.");
    } else {
        // Plaintext is no longer a valid storage mode, but the label
        // still says what to do if crypto is disabled so the user
        // understands why the token reads as "not set".
        const char *tok_status;
        // Status only — no decrypt here; that is ~2 s of PBKDF2 on the UI
        // thread (P7.3).
        if (!token_is_encrypted())               tok_status = "token: not set";
        else if (notes_crypto_is_unlocked())     tok_status = "token: encrypted, unlocked";
        else                                     tok_status = "token: locked";
        lv_label_set_text_fmt(status, "%s  (%s)",
                              repo.c_str(), tok_status);
    }

    // Scrolling log area — the sync worker's progress and summary lines.
    s_log_scroll = lv_obj_create(parent);
    lv_obj_set_width(s_log_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_log_scroll, 1);
    lv_obj_set_style_bg_color(s_log_scroll, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_log_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_log_scroll, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_log_scroll, 1, 0);
    lv_obj_set_style_radius(s_log_scroll, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(s_log_scroll, 4, 0);
    lv_obj_set_scroll_dir(s_log_scroll, LV_DIR_VER);

    s_log_label = lv_label_create(s_log_scroll);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_log_label, lv_pct(100));
    lv_obj_set_style_text_color(s_log_label, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(s_log_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_log_label, "");
    lv_obj_add_event_cb(s_log_label, log_size_changed_cb, LV_EVENT_SIZE_CHANGED, nullptr);
}

static void exit_cb() {
    ui_hide_back_button();
    s_log_label = s_log_scroll = nullptr;
    s_log_text.clear();
    // Stop draining; the worker (if any) is left to finish. It only writes to
    // the heap-backed log queue, never the UI tree, so it is safe to outlive
    // the app instance. Drop queued log entries since there's no UI to render
    // them into.
    if (s_bg_timer) {
        lv_timer_del(s_bg_timer);
        s_bg_timer = nullptr;
    }
    s_sync_job.teardown();
    bg_log_clear();
    s_syncing = false;
}

#ifdef ARDUINO
static void on_unlocked_cb(bool ok, void *ud)
{
    // The enter() path reads the banner state from NVS + session; after
    // unlock we just rebuild the page so the banner flips from "locked"
    // to "encrypted" without a kick back to the menu. An unsuccessful
    // unlock leaves the banner as-is and Sync now refuses with a clear
    // message — no value in kicking back out of the app.
    lv_obj_t *parent = (lv_obj_t *)ud;
    if (!parent) return;
    lv_obj_clean(parent);
    enter(parent);
    if (ok) start_sync();
}
#endif

class NotesSyncApp : public core::App {
public:
    NotesSyncApp() : core::App("Notes Sync") {}
    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        enter(parent);
#ifdef ARDUINO
        // Offer to unlock the notes session when the token is at rest
        // encrypted but the session is still locked. ui_passphrase_unlock
        // is a no-op (fires the callback immediately) when crypto is
        // disabled or already unlocked, so the normal case costs nothing.
        // Phone path needs no unlock (ciphertext travels as-is).
        if (!hw_link_connected() && token_is_encrypted() && !notes_crypto_is_unlocked()) {
            ui_passphrase_unlock(on_unlocked_cb, parent);
        } else {
            start_sync();
        }
#else
        start_sync();
#endif
    }
    void onStop() override {
        exit_cb();
        core::App::onStop();
    }
};

} // namespace

namespace apps {

APP_FACTORY(make_notes_sync_app, NotesSyncApp)

// --- config helpers called from settings_notes_sync.cpp -------------------
std::string nsync_cfg_get_repo()            { return load_pref("repo"); }
void        nsync_cfg_set_repo(const char *v)    { save_pref("repo", v ? v : ""); }

std::string nsync_cfg_get_branch()
{
    std::string v = load_pref("branch", NSYNC_DEFAULT_BRANCH);
    if (v.empty()) v = NSYNC_DEFAULT_BRANCH;
    return v;
}
void nsync_cfg_set_branch(const char *v) { save_pref("branch", v ? v : ""); }

std::string nsync_cfg_get_token_display()
{
    return hal::secret_mask_display(NSYNC_PREFS_NS, "token_enc");
}

// Cheap form for the UI thread — see hal::secret_mask_peek.
bool nsync_cfg_peek_token_display(std::string &out)
{
    return hal::secret_mask_peek(NSYNC_PREFS_NS, "token_enc", out);
}

bool nsync_cfg_set_token(const char *tok, std::string *err)
{
    return save_token(tok ? tok : "", err);
}

bool nsync_cfg_token_is_encrypted()         { return token_is_encrypted(); }

} // namespace apps
