/**
 * @file      ui_telegram.cpp
 * @brief     Telegram client via tg-bridge (HTTPS + bearer).
 *
 * Single-account polling client. The bridge (a Go service the user runs on
 * a Raspberry Pi or VPS) holds the MTProto session; this app only speaks
 * a narrow JSON/bearer API to it. No WebSocket client on-device — we poll
 * /v1/chats and /v1/chats/{id}/messages on an LVGL timer while the
 * relevant view is open.
 *
 * Bridge URL and bearer token are managed from the Settings app
 * (Settings → Telegram). The cfg helpers at the bottom of this file are
 * what the settings subpage calls into.
 *
 * Text rendering: LVGL's UTF-8 path looks up glyphs by codepoint and skips
 * missing ones, so non-ASCII characters survive if the selected font has
 * the glyph. The Inter font (idx 4) covers Latin-1 (á, é, ã, ç, …) — it's
 * the default for Telegram so Portuguese messages render out of the box.
 *
 * State reset on onStop:
 *   widgets: s_root, s_list_holder, s_msgs_holder, s_status_label, s_input_ta
 *                                       — nulled (deleted with parent)
 *   timers:  s_timer (poll scheduler)   — lv_timer_del + null
 *            s_drain_timer (result drain)— lv_timer_del + null
 *   tasks:   s_bg_task (HTTPS worker)    — left to self-delete; the epoch bump
 *                                          makes any late result be discarded
 *   note:    s_bg_timer is the global background notifier (fast: 60 s / slow: 5 min)
 *            created once at boot by tg_begin_background_poll(); it is NOT torn
 *            down here.  onStart() snaps its period back to TG_BG_POLL_FAST_MS.
 *
 * The foreground poll / send / mark-read run on a one-shot worker (§2.1):
 * it only writes heap state under the instance mutex, never the LVGL tree, so
 * it is safe to outlive the app instance — the drain timer applies the result
 * back on the UI thread. Previously these ran full HTTPS round-trips inline on
 * the LVGL thread, freezing the UI ~1-2 s per poll and per keystroke-send.
 * If you add a cached LVGL pointer or a task, list it above AND extend onStop().
 */
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../hal/system.h"
#include "../hal/notes_crypto.h"
#include "../hal/secrets.h"
#include "../hal/hub.h"
#include "../hal/nvs.h"
#include "../hal/str_encode.h"
#include "../core/app.h"
#include "../core/app_manager.h"
#include "../core/notify.h"
#include "../core/system.h"
#include "../core/input_focus.h"
#include "app_registry.h"
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../core/scoped_lock.h"
extern "C" {
#include "cJSON.h"
}
#endif

namespace {

#define TG_PREFS_NS       "tgbridge"
#define TG_LIST_POLL_MS   10000
#define TG_CHAT_POLL_MS   5000
#define TG_CHAT_LIMIT     30
#define TG_MSG_LIMIT      20
#define TG_DRAIN_MS         120     // async-worker result poll (see §2.1 worker)
#define TG_BG_POLL_FAST_MS  60000   // background poll: 60 s while user active
#define TG_BG_POLL_SLOW_MS  300000  // background poll: 5 min while user idle
#define TG_BG_IDLE_MS       180000  // inactivity threshold for slow cadence (3 min)

// --- persisted config ------------------------------------------------------

// Thin namespace-binding wrappers over the shared hal::nvs_* helpers; the
// begin/get/put/end boilerplate (and its emulator stub) lives in hal/nvs.cpp.
static std::string load_pref(const char *key)            { return hal::nvs_get_str(TG_PREFS_NS, key); }
static void        save_pref(const char *key, const char *value) { hal::nvs_set_str(TG_PREFS_NS, key, value); }
static bool        load_bool_pref(const char *key, bool dflt)     { return hal::nvs_get_bool(TG_PREFS_NS, key, dflt); }
static void        save_bool_pref(const char *key, bool value)    { hal::nvs_set_bool(TG_PREFS_NS, key, value); }

// The bearer lives in NVS as `token_enc` (AES-256-CBC + PBKDF2, OpenSSL-enc
// format) produced by notes_crypto. We piggyback on the notes passphrase —
// one unlock per session covers every secret slot device-wide. Persistence
// delegated to hal/secrets; legacy plaintext `token` slot is wiped at boot.
static bool token_is_encrypted() {
    return hal::secret_exists(TG_PREFS_NS, "token_enc");
}

static std::string load_token_plain() {
    return hal::secret_load(TG_PREFS_NS, "token_enc");
}

// secret_store trims the pasted whitespace that would break every Bot API URL.
static bool save_token(const char *value, std::string *err) {
    bool ok = hal::secret_store(TG_PREFS_NS, "token_enc", value, err);
    // Clearing or rewriting the encrypted slot also nukes any legacy
    // plaintext sibling so the two never coexist.
    if (ok) hal::secret_purge_legacy(TG_PREFS_NS, "token");
    return ok;
}

// --- state ----------------------------------------------------------------

struct Chat {
    long long id;
    std::string title;
    int unread;
};

struct Message {
    int id;
    std::string from;
    std::string text;
    bool out;
    // Bridge sends a "media" object for non-text messages (photo, video,
    // voice, document, sticker, other). We don't download the payload yet —
    // just surface a placeholder line in the bubble so the chat doesn't look
    // empty when an image arrives.
    std::string media_type;
    int media_w;
    int media_h;
    // Font-filtered (ascii_safe) copies of from/text, computed once at parse
    // time on the fetch worker (see tg_parse_msgs) instead of on every
    // render_msgs() call.
    std::string safe_from;
    std::string safe_text;
};

static lv_obj_t *s_root = nullptr;
static lv_timer_t *s_timer = nullptr;
static std::string s_base_url;
static std::string s_auth_header;     // "Bearer <token>" or empty

enum View { V_NONE, V_LIST, V_CHAT, V_NOT_CONFIGURED };
static View s_view = V_NONE;

static long long s_current_chat_id = 0;
static std::string s_current_chat_title;
// Highest message id we've asked the bridge to mark as read in the current
// chat view. Reset to 0 whenever we open a new chat so the first fetch
// always fires the mark-read POST.
static int s_last_marked_msg_id = 0;

static lv_obj_t *s_list_holder = nullptr;
static lv_obj_t *s_msgs_holder = nullptr;
// True while the user has clicked into the history to scroll it. Polling is
// paused in this state because a refetch re-renders and pins scroll to the
// bottom, yanking the view back down while the user is trying to scroll up.
static bool s_msgs_scroll_mode = false;
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_input_ta = nullptr;
static int32_t   s_input_expanded_h = 0;

static std::vector<Chat> s_chats;
static std::vector<Message> s_msgs;

// §2.6: cheap signatures of the last-rendered chat list / message list so a
// poll whose data is identical skips the full lv_obj_clean + rebuild (and, for
// the chat list, avoids reseating the &s_chats[i] button user-data). Reset to
// invalid whenever a page is (re)built so the first render always happens.
static bool     s_chats_sig_valid = false;
static bool     s_msgs_sig_valid = false;
#ifdef ARDUINO
static uint32_t s_chats_sig = 0;
static uint32_t s_msgs_sig = 0;
#endif

// Favorite chat IDs. The chat list in the Telegram app filters to this set;
// the full list is only visible from Settings → Telegram → Favorites.
static std::set<long long> s_favorites;
// Cached titles parallel to s_favorites. Lets the app skip the chat list
// and jump straight into the sole favorite without first fetching /v1/chats
// to learn the title.
static std::map<long long, std::string> s_favorite_titles;
static bool s_favorites_loaded = false;

// Unread-count state shared with the home-menu badge. Updated by the in-app
// poll when the Telegram view is open, and by the background poll otherwise
// (see tg_bg_timer below).
static int s_unread_total = 0;

#ifdef ARDUINO
static lv_timer_t *s_bg_timer = nullptr;

// Baseline for the background notifier. -1 means "not initialized yet": the
// first successful bg poll seeds it so we don't buzz on boot for messages
// that arrived while the device was off. Subsequent polls fire enabled
// notifiers only when `sum > s_last_notified_unread`.
static int s_last_notified_unread = -1;

// s_bg_task is the single HTTPS worker slot, shared between the background
// unread notifier and the foreground poll/send worker so the two never run a
// TLS session at the same time (each costs several KB of internal heap).
static TaskHandle_t s_bg_task = nullptr;
// Mirrors the period currently set on s_bg_timer so tg_bg_tick() can detect
// when it needs to change without a second lv_timer API call.
// Also kept in sync by onStart() which snaps back to the fast cadence on open.
static uint32_t s_bg_timer_period = 0;

// --- foreground async worker state (§2.1) ---------------------------------
enum FgOp { FG_FETCH_CHATS, FG_FETCH_MSGS, FG_SEND };

// Handed to the worker (heap-owned by the task, freed there). base_url/auth are
// captured at kick time so the worker never reads s_base_url / s_auth_header,
// which onStop scrubs and which may be gone before the worker finishes.
struct FgReq {
    FgOp        op;
    std::string base_url;
    std::string auth;         // "Bearer <token>"
    long long   chat_id;      // FG_FETCH_MSGS / FG_SEND
    int         mark_from;    // FG_FETCH_MSGS: snapshot of s_last_marked_msg_id
    std::string send_text;    // FG_SEND
    uint32_t    epoch;        // discarded on the far side if it no longer matches
};

// Handed back to the UI thread (heap-owned, freed in the drain tick).
struct FgResult {
    FgOp        op;
    bool        ok;
    std::string status;       // status-pill text ("" clears it)
    lv_color_t  status_color;
    long long   chat_id;      // echo for the FG_FETCH_MSGS/SEND relevance check
    std::vector<Chat>    chats;    // FG_FETCH_CHATS
    int         unread_total;
    std::vector<Message> msgs;     // FG_FETCH_MSGS
    int         marked_to;    // FG_FETCH_MSGS: new mark-read high-water (0 = none)
};

static lv_timer_t   *s_drain_timer = nullptr;
static volatile bool s_fg_done   = false;
static FgResult     *s_fg_result = nullptr;   // heap-owned; the drain frees it
// Bumped by onStop/onStart; a worker whose captured epoch no longer matches
// discards its result instead of publishing into a torn-down / fresh view.
static uint32_t      s_fg_epoch  = 0;
// A send the user fired while the worker was busy with a poll fetch. Queued
// here so a keystroke-send is never dropped; the drain launches it once free.
static std::string   s_pending_send;
static long long     s_pending_send_chat = 0;
#endif

// --- forward decls --------------------------------------------------------

static void show_chat_list();
static void show_chat(long long id, const char *title);
static void show_not_configured();

// --- helpers --------------------------------------------------------------

// Favorites: ids in `favs` (comma-separated) and titles in `fav_titles`
// (`<id>\x1F<title>\x1E` records — separators that telegram chat titles
// won't contain). Title pref is best-effort; missing entries leave the
// chat-name pill blank but don't otherwise break anything.
static void load_favorites()
{
    s_favorites.clear();
    s_favorite_titles.clear();
    s_favorites_loaded = true;
    std::string raw = load_pref("favs");
    if (!raw.empty()) {
        size_t i = 0;
        while (i < raw.size()) {
            size_t j = raw.find(',', i);
            std::string tok = raw.substr(i, j == std::string::npos ? std::string::npos : j - i);
            if (!tok.empty()) {
                long long v = strtoll(tok.c_str(), nullptr, 10);
                if (v != 0) s_favorites.insert(v);
            }
            if (j == std::string::npos) break;
            i = j + 1;
        }
    }
    std::string traw = load_pref("fav_titles");
    if (!traw.empty()) {
        size_t i = 0;
        while (i < traw.size()) {
            size_t end = traw.find('\x1E', i);
            std::string rec = traw.substr(i, end == std::string::npos ? std::string::npos : end - i);
            size_t sep = rec.find('\x1F');
            if (sep != std::string::npos) {
                long long id = strtoll(rec.substr(0, sep).c_str(), nullptr, 10);
                if (id != 0 && s_favorites.count(id)) {
                    s_favorite_titles[id] = rec.substr(sep + 1);
                }
            }
            if (end == std::string::npos) break;
            i = end + 1;
        }
    }
}

static void save_favorites()
{
    std::string joined;
    joined.reserve(s_favorites.size() * 12);
    for (auto it = s_favorites.begin(); it != s_favorites.end(); ++it) {
        if (!joined.empty()) joined.push_back(',');
        char b[24];
        snprintf(b, sizeof(b), "%lld", *it);
        joined += b;
    }
    save_pref("favs", joined.c_str());

    std::string blob;
    for (long long id : s_favorites) {
        auto it = s_favorite_titles.find(id);
        if (it == s_favorite_titles.end() || it->second.empty()) continue;
        char b[24];
        snprintf(b, sizeof(b), "%lld", id);
        blob += b;
        blob.push_back('\x1F');
        blob += it->second;
        blob.push_back('\x1E');
    }
    save_pref("fav_titles", blob.c_str());
}

static void reload_config()
{
    s_base_url = load_pref("url");
    std::string tok = load_token_plain();
    s_auth_header = tok.empty() ? std::string() : ("Bearer " + tok);
    if (!s_favorites_loaded) load_favorites();
}

static bool configured()
{
    return !s_base_url.empty() && !s_auth_header.empty();
}

// Floating label with a surface background behind the text only, used for
// the status and chat-title pills. FLOATING keeps it out of the parent's
// flex column so it overlays the list / history.
static lv_obj_t *make_pill(lv_obj_t *parent, const char *text,
                           lv_align_t align, int32_t x_ofs, int32_t y_ofs)
{
    lv_obj_t *pill = lv_label_create(parent);
    lv_label_set_text(pill, text);
    lv_obj_set_style_text_font(pill, get_telegram_font(), 0);
    lv_obj_set_style_text_color(pill, UI_COLOR_FG, 0);
    lv_obj_set_style_bg_color(pill, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(pill, 4, 0);
    lv_obj_set_style_pad_ver(pill, 1, 0);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(pill, align, x_ofs, y_ofs);
    return pill;
}

static void set_status(const char *text, lv_color_t color)
{
    if (!s_status_label) return;
    const bool empty = !text || !*text;
    lv_label_set_text(s_status_label, text ? text : "");
    lv_obj_set_style_text_color(s_status_label, color, 0);
    if (empty) lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
}

// Drop any codepoint the Telegram font (Inter + emoji fallback) can't
// render so the label doesn't show a tofu rectangle for it. Ask LVGL
// directly: lv_font_get_glyph_dsc walks the fallback chain, so ASCII +
// Latin-1 go through Inter, curated emoji through font_emoji_*, and
// anything else — CJK, rare symbols, unsupported emoji — is elided.
//
// Also strips bare control bytes (except \n/\t) and discards malformed
// UTF-8.
//
// Called on the UI thread (chat list / title pill) AND on the fg worker
// (tg_parse_msgs). The worker call is safe in practice: every font
// get_telegram_font() can return is a const lv_font_fmt_txt font (LVGL 9's
// fmt_txt glyph lookup keeps no mutable cache), so lv_font_get_glyph_dsc
// only reads constant tables. Caveats: get_telegram_font() lazily builds the
// Inter+emoji fallback chain on first use (unsynchronised, but always first
// reached from the UI thread when the view is built) and copies the settings
// struct without a lock.
static std::string ascii_safe(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    const lv_font_t *font = get_telegram_font();
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20) {
            if (c == '\n' || c == '\t') out.push_back((char)c);
            i++; continue;
        }
        size_t extra;
        uint32_t cp;
        if (c < 0x80)                { out.push_back((char)c); i++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else { i++; continue; }
        if (i + extra >= in.size()) break;
        bool bad = false;
        for (size_t k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!bad) {
            lv_font_glyph_dsc_t dsc;
            if (font && lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                out.append(in, i, 1 + extra);
            }
        }
        i += 1 + extra;
    }
    return out;
}

#ifdef ARDUINO
// Serialize a single {"text":"..."} JSON body.
static std::string json_text_body(const char *text)
{
    return "{\"text\":\"" + hal::json_escape(text ? text : "") + "\"}";
}

// --- HTTP -----------------------------------------------------------------

// Cached internet reachability. WiFi-associated does not imply the bridge is
// reachable (captive portals, ISP outage, DNS down), and the bridge HTTPS
// timeout is multi-second — so we probe TCP/53 to 1.1.1.1 and cache the
// verdict. Success is cached longer than failure so a recovering link is
// retried sooner without spamming the probe.
//
// The probe blocks for up to TG_INET_PING_MS, so it runs on a one-shot
// background task; internet_available() only ever reads the cached verdict and
// kicks a refresh when it goes stale. Previously the probe ran inline on the
// LVGL thread, so a WiFi-up/internet-down link janked the UI ~1.5 s every
// FAIL_TTL forever. Entry paths that dead-end their UI on a false result keep a
// short retry timer (TG_INET_RETRY_MS) so they recover once the async probe flips.
static const uint32_t TG_INET_OK_TTL_MS   = 30000;
static const uint32_t TG_INET_FAIL_TTL_MS = 5000;
static const uint32_t TG_INET_PING_MS     = 1500;
static const uint32_t TG_INET_RETRY_MS    = 1500;
static uint32_t s_inet_check_ts = 0;
static bool     s_inet_ok       = false;
static volatile bool s_inet_probe_running = false;

static void inet_probe_task(void *arg)
{
    (void)arg;
    bool ok = hw_ping_internet("1.1.1.1", 53, TG_INET_PING_MS, nullptr, nullptr);
    s_inet_ok = ok;
    s_inet_check_ts = lv_tick_get();
    s_inet_probe_running = false;
    vTaskDelete(NULL);
}

// Spawn a background probe unless one is already in flight. No-op without WiFi.
static void kick_inet_probe()
{
    if (s_inet_probe_running) return;
    if (!hw_get_wifi_connected()) return;
    s_inet_probe_running = true;
    if (xTaskCreate(inet_probe_task, "tg_inet", 3072, nullptr, 1, nullptr) != pdPASS) {
        // Task slots exhausted — probe inline so the verdict still refreshes.
        s_inet_ok = hw_ping_internet("1.1.1.1", 53, TG_INET_PING_MS, nullptr, nullptr);
        s_inet_check_ts = lv_tick_get();
        s_inet_probe_running = false;
    }
}

// Non-blocking: returns the last cached verdict and refreshes it in the
// background when stale. Safe to call from the LVGL thread on every poll tick.
static bool internet_available()
{
    if (!hw_get_wifi_connected()) {
        s_inet_ok = false;
        s_inet_check_ts = lv_tick_get();
        return false;
    }
    uint32_t ttl = s_inet_ok ? TG_INET_OK_TTL_MS : TG_INET_FAIL_TTL_MS;
    bool fresh = (s_inet_check_ts != 0 && lv_tick_elaps(s_inet_check_ts) < ttl);
    if (!fresh) kick_inet_probe();   // refresh in the background; never blocks here
    return s_inet_ok;
}

static bool tg_http_request(const std::string &url, const char *method,
                            const char *body, size_t body_len,
                            const char *content_type, const char *auth_header,
                            std::string &out_body, int *out_code, std::string *err)
{
#ifdef ARDUINO
    // hub_last_reachable() is a non-blocking read of the status-bar probe's
    // cached verdict (it already checks hub_is_enabled()); the old
    // hub_is_reachable() here did a 1.5 s blocking connect() on the LVGL thread
    // on every poll/send/mark-read. A stale/unknown verdict falls through to the
    // direct Telegram path below.
    if (hal::hub_last_reachable()) {
        std::string proxy_url = hal::hub_get_url() + "/api/telegram/proxy";
        
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "url", url.c_str());
        cJSON_AddStringToObject(req, "method", method);
        if (auth_header && strlen(auth_header) > 7) {
            cJSON_AddStringToObject(req, "token", auth_header + 7);
        }
        if (body && body_len > 0) {
            std::string bs(body, body_len);
            cJSON_AddStringToObject(req, "body", bs.c_str());
        }
        char *req_str = cJSON_PrintUnformatted(req);
        std::string req_body;
        if (req_str) {
            req_body = req_str;
            free(req_str);
        }
        cJSON_Delete(req);
        
        if (!req_body.empty()) {
            int pcode = 0;
            if (hw_http_request(proxy_url.c_str(), "POST",
                                req_body.c_str(), req_body.size(),
                                "application/json", nullptr,
                                out_body, &pcode, err)) {
                if (out_code) *out_code = pcode;
                return true;
            }
            // P6.28: the hub proxy answers 4xx for a URL it won't forward (its
            // allowlist predates the bridge API), and a transport failure means
            // the hub went away — retry both directly. A 5xx is returned as-is:
            // the hub may already have forwarded a POST (send / mark-read).
            if (pcode >= 500) {
                if (out_code) *out_code = pcode;
                return false;
            }
            out_body.clear();
        }
    }
#endif
    return hw_http_request(url.c_str(), method, body, body_len,
                           content_type, auth_header, out_body, out_code, err);
}

// --- chat list ------------------------------------------------------------
// Rendering is driven by tg_drain_tick (hardware-only worker results).

static void chat_click_cb(lv_event_t *e)
{
    auto *chat = (Chat *)lv_event_get_user_data(e);
    if (!chat) return;
    hw_feedback();
    show_chat(chat->id, chat->title.c_str());
}

static void render_chats()
{
    if (!s_list_holder) return;
    lv_obj_clean(s_list_holder);
    size_t shown = 0;
    for (size_t i = 0; i < s_chats.size(); i++) {
        Chat &c = s_chats[i];
        if (s_favorites.find(c.id) == s_favorites.end()) continue;
        std::string label = ascii_safe(c.title);
        if (c.unread > 0) {
            char b[16];
            snprintf(b, sizeof(b), " (%d)", c.unread);
            label += b;
        }
        lv_obj_t *btn = lv_list_add_button(s_list_holder, NULL, label.c_str());
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(btn, get_telegram_list_font(), 0);
        lv_obj_add_event_cb(btn, chat_click_cb, LV_EVENT_CLICKED, &c);
        shown++;
    }
    if (shown == 0) {
        lv_obj_t *hint = lv_label_create(s_list_holder);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_label_set_text(hint,
            "No favorites yet.\n"
            "Add chats in Settings " "\xC2\xBB" " Telegram " "\xC2\xBB" " Favorites.");
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_text_color(hint, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_font(hint, get_telegram_font(), 0);
        lv_obj_set_style_pad_all(hint, 6, 0);
    }
}

// --- chat view (messages + send) ------------------------------------------

static void render_msgs()
{
    if (!s_msgs_holder) return;
    lv_obj_clean(s_msgs_holder);
    // API returns newest-first; display oldest-at-top so the newest sits at
    // the bottom where the user expects.
    for (auto it = s_msgs.rbegin(); it != s_msgs.rend(); ++it) {
        // Transparent full-width wrapper whose flex alignment anchors the
        // bubble to the left (incoming) or right (own messages).
        lv_obj_t *wrap = lv_obj_create(s_msgs_holder);
        lv_obj_set_width(wrap, lv_pct(100));
        lv_obj_set_height(wrap, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wrap, 0, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(wrap,
            it->out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
            LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        lv_obj_t *row = lv_obj_create(wrap);
        lv_obj_set_width(row, lv_pct(85));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, it->out ? lv_color_mix(UI_COLOR_ACCENT, UI_COLOR_SURFACE, LV_OPA_40)
                                               : lv_color_mix(UI_COLOR_FG, UI_COLOR_SURFACE, LV_OPA_10), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

        if (!it->out && !it->from.empty()) {
            lv_obj_t *from = lv_label_create(row);
            lv_label_set_text(from, it->safe_from.c_str());
            lv_obj_set_style_text_color(from, UI_COLOR_ACCENT, 0);
            lv_obj_set_style_text_font(from, get_telegram_font(), 0);
        }

        if (!it->media_type.empty()) {
            const char *label;
            if      (it->media_type == "photo")    label = "[Photo]";
            else if (it->media_type == "video")    label = "[Video]";
            else if (it->media_type == "voice")    label = "[Voice]";
            else if (it->media_type == "sticker")  label = "[Sticker]";
            else if (it->media_type == "document") label = "[Document]";
            else                                   label = "[Media]";
            char buf[48];
            if (it->media_w > 0 && it->media_h > 0) {
                snprintf(buf, sizeof(buf), "%s %dx%d", label, it->media_w, it->media_h);
            } else {
                snprintf(buf, sizeof(buf), "%s", label);
            }
            lv_obj_t *mlbl = lv_label_create(row);
            lv_label_set_text(mlbl, buf);
            lv_obj_set_style_text_color(mlbl, UI_COLOR_MUTED, 0);
            lv_obj_set_style_text_font(mlbl, get_telegram_font(), 0);
        }

        if (!it->text.empty()) {
            lv_obj_t *t = lv_label_create(row);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
            lv_label_set_text(t, it->safe_text.c_str());
            lv_obj_set_width(t, lv_pct(100));
            lv_obj_set_style_text_color(t, UI_COLOR_FG, 0);
            lv_obj_set_style_text_font(t, get_telegram_font(), 0);
        }
    }
    lv_obj_scroll_to_y(s_msgs_holder, LV_COORD_MAX, LV_ANIM_OFF);
}

static void tg_drain_tick(lv_timer_t *t);   // fwd (defined after the kickers)

// Load the bridge URL and build the "Bearer <token>" header from NVS (worker
// safe). On failure `auth` is empty and *err says what is missing; the token
// plaintext never outlives this call except inside `auth`, which the caller
// must hal::secret_scrub().
static bool load_bridge_auth(std::string &url, std::string &auth, std::string *err)
{
    url = load_pref("url");
    std::string tok = load_token_plain();
    if (url.empty()) {
        hal::secret_scrub(tok);
        if (err) *err = "Bridge URL not set.";
        return false;
    }
    if (tok.empty()) {
        if (err) *err = token_is_encrypted()
                        ? "Token locked — open Notes to unlock."
                        : "Bearer token not set.";
        return false;
    }
    auth = "Bearer " + tok;
    hal::secret_scrub(tok);  // plaintext is now only inside `auth`
    return true;
}

// GET <base_url>/v1/chats through tg_http_request (hub proxy first, direct
// fallback) and parse it into `out`. Worker-safe: no LVGL, no shared state.
// *unread_total (optional) gets the sum of the positive per-chat unread counts.
static bool fetch_chats(std::string base_url, const std::string &auth,
                        std::vector<Chat> &out, int *unread_total,
                        std::string *err)
{
    out.clear();
    if (unread_total) *unread_total = 0;
    if (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
    char path[64];
    snprintf(path, sizeof(path), "/v1/chats?limit=%d", TG_CHAT_LIMIT);
    std::string body; int code = 0;
    if (!tg_http_request(base_url + path, "GET", nullptr, 0, nullptr,
                         auth.c_str(), body, &code, err)) {
        return false;
    }
    cJSON *arr = cJSON_Parse(body.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        if (err) *err = "Parse error";
        return false;
    }
    int n = cJSON_GetArraySize(arr);
    out.reserve((size_t)n);
    int unread_sum = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        Chat c;
        cJSON *jid  = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jti  = cJSON_GetObjectItemCaseSensitive(it, "title");
        cJSON *junr = cJSON_GetObjectItemCaseSensitive(it, "unread");
        c.id     = (jid  && cJSON_IsNumber(jid))  ? (long long)jid->valuedouble : 0;
        c.title  = (jti  && cJSON_IsString(jti))  ? jti->valuestring : "(no title)";
        c.unread = (junr && cJSON_IsNumber(junr)) ? (int)junr->valuedouble : 0;
        if (c.unread > 0) unread_sum += c.unread;
        out.push_back(std::move(c));
    }
    cJSON_Delete(arr);
    if (unread_total) *unread_total = unread_sum;
    return true;
}

// Parse a /v1/chats/{id}/messages array (worker thread) into res->msgs.
// Returns the newest message id seen (for the mark-read decision).
static int tg_parse_msgs(cJSON *arr, FgResult *res)
{
    int n = cJSON_GetArraySize(arr);
    res->msgs.reserve((size_t)n);
    int newest_id = 0;
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_GetArrayItem(arr, i);
        Message m;
        cJSON *jid  = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jtxt = cJSON_GetObjectItemCaseSensitive(it, "text");
        cJSON *jfr  = cJSON_GetObjectItemCaseSensitive(it, "from");
        cJSON *jout = cJSON_GetObjectItemCaseSensitive(it, "out");
        m.id   = (jid  && cJSON_IsNumber(jid))  ? (int)jid->valuedouble : 0;
        m.text = (jtxt && cJSON_IsString(jtxt)) ? jtxt->valuestring : "";
        m.from = (jfr  && cJSON_IsString(jfr))  ? jfr->valuestring : "";
        m.out  = (jout && cJSON_IsBool(jout))   ? cJSON_IsTrue(jout) : false;
        m.media_w = 0;
        m.media_h = 0;
        cJSON *jmd = cJSON_GetObjectItemCaseSensitive(it, "media");
        if (jmd && cJSON_IsObject(jmd)) {
            cJSON *jmt = cJSON_GetObjectItemCaseSensitive(jmd, "type");
            cJSON *jmw = cJSON_GetObjectItemCaseSensitive(jmd, "w");
            cJSON *jmh = cJSON_GetObjectItemCaseSensitive(jmd, "h");
            if (jmt && cJSON_IsString(jmt)) m.media_type = jmt->valuestring;
            if (jmw && cJSON_IsNumber(jmw)) m.media_w = (int)jmw->valuedouble;
            if (jmh && cJSON_IsNumber(jmh)) m.media_h = (int)jmh->valuedouble;
        }
        // Sanitize once here (worker thread) instead of on every render_msgs()
        // call — glyph-table lookups are the same cost either way, but this
        // way they happen once per fetch instead of once per render.
        m.safe_from = ascii_safe(m.from);
        m.safe_text = ascii_safe(m.text);
        if (m.id > newest_id) newest_id = m.id;
        res->msgs.push_back(std::move(m));
    }
    return newest_id;
}

// The HTTPS worker. Runs one request (chats / messages / send), parses into a
// heap FgResult, and publishes it under the instance mutex for the drain tick.
// Never touches the LVGL tree (parsing produces plain strings; only rendering
// happens on the UI thread), so it is safe to outlive the app. tg_parse_msgs
// does run ascii_safe here, i.e. LVGL glyph lookups — see that function.
static void tg_fg_task(void *arg)
{
    FgReq *req = (FgReq *)arg;
    FgResult *res = new FgResult();
    res->op           = req->op;
    res->ok           = false;
    res->chat_id      = req->chat_id;
    res->unread_total = 0;
    res->marked_to    = 0;
    res->status_color = UI_COLOR_MUTED;

    std::string base = req->base_url;
    if (!base.empty() && base.back() == '/') base.pop_back();

    if (req->op == FG_FETCH_CHATS) {
        std::string err;
        if (fetch_chats(base, req->auth, res->chats, &res->unread_total, &err)) {
            res->ok = true;
        } else {
            res->status = err;
            res->status_color = lv_palette_main(LV_PALETTE_RED);
        }
    } else if (req->op == FG_FETCH_MSGS) {
        char path[80];
        snprintf(path, sizeof(path), "/v1/chats/%lld/messages?limit=%d",
                 req->chat_id, TG_MSG_LIMIT);
        std::string body, err; int code = 0;
        if (!tg_http_request(base + path, "GET", nullptr, 0, nullptr,
                             req->auth.c_str(), body, &code, &err)) {
            res->status = err;
            res->status_color = lv_palette_main(LV_PALETTE_RED);
        } else {
            cJSON *arr = cJSON_Parse(body.c_str());
            if (!arr || !cJSON_IsArray(arr)) {
                res->status = "Parse error";
                res->status_color = lv_palette_main(LV_PALETTE_RED);
            } else {
                int newest_id = tg_parse_msgs(arr, res);
                res->ok = true;
                // Fire-and-forget mark-read once the newest id advances past
                // what the UI last acked (req->mark_from). Done here so the UI
                // thread never blocks on the extra POST; the new high-water
                // mark rides back in res->marked_to for the drain to record.
                if (req->chat_id != 0 && newest_id > 0 &&
                    newest_id > req->mark_from) {
                    char rpath[48];
                    snprintf(rpath, sizeof(rpath), "/v1/chats/%lld/read",
                             req->chat_id);
                    char rbody[48];
                    snprintf(rbody, sizeof(rbody), "{\"up_to\":%d}", newest_id);
                    std::string rresp, rerr; int rcode = 0;
                    if (tg_http_request(base + rpath, "POST", rbody,
                                        strlen(rbody), "application/json",
                                        req->auth.c_str(), rresp, &rcode,
                                        &rerr)) {
                        res->marked_to = newest_id;
                    }
                }
            }
            if (arr) cJSON_Delete(arr);
        }
    } else { // FG_SEND
        std::string bodyj = json_text_body(req->send_text.c_str());
        char path[48];
        snprintf(path, sizeof(path), "/v1/chats/%lld/messages", req->chat_id);
        std::string resp, err; int code = 0;
        if (!tg_http_request(base + path, "POST", bodyj.c_str(), bodyj.size(),
                             "application/json", req->auth.c_str(), resp,
                             &code, &err)) {
            res->status = "send: " + err;
            res->status_color = lv_palette_main(LV_PALETTE_RED);
        } else {
            res->ok = true;
            res->status = "Sent";
        }
    }

    // Publish under the instance mutex — the drain tick reads these under the
    // same lock. Discard if the app moved on (epoch bumped by onStop/onStart)
    // so a late worker can't flash a result into a torn-down / fresh view.
    {
        core::ScopedInstanceLock lock;
        if (req->epoch == s_fg_epoch) {
            if (s_fg_result) { delete s_fg_result; s_fg_result = nullptr; }
            s_fg_result = res;
            s_fg_done   = true;
            res = nullptr;
        }
        s_bg_task = nullptr;
    }
    if (res) delete res;
    hal::secret_scrub(req->auth);
    delete req;
    vTaskDelete(NULL);
}

static void ensure_drain_timer()
{
    if (!s_drain_timer)
        s_drain_timer = lv_timer_create(tg_drain_tick, TG_DRAIN_MS, nullptr);
}

// Snapshot the config + chat cursor into a heap request for the worker.
static FgReq *make_req(FgOp op)
{
    FgReq *r = new FgReq();
    r->op        = op;
    r->base_url  = s_base_url;
    r->auth      = s_auth_header;   // "Bearer <token>"
    r->chat_id   = s_current_chat_id;
    r->mark_from = s_last_marked_msg_id;
    r->epoch     = s_fg_epoch;
    return r;
}

// Spawn the worker for `req` (which it takes ownership of). Returns false —
// after freeing req — if the single worker slot is busy or the spawn fails.
static bool kick_worker(FgReq *req)
{
    if (s_bg_task != nullptr) { hal::secret_scrub(req->auth); delete req; return false; }
    // 8 KB stack: the mbedtls TLS handshake + cert chain dominates, same as the
    // weather worker; parsing 30 chats / 20 messages is cheap next to it.
    if (xTaskCreate(tg_fg_task, "tg_fg", 8192, req, 2, &s_bg_task) != pdPASS) {
        s_bg_task = nullptr;
        hal::secret_scrub(req->auth);
        delete req;
        return false;
    }
    ensure_drain_timer();
    return true;
}

static bool kick_fetch_chats()
{
    if (s_chats.empty()) set_status("Loading...", UI_COLOR_ACCENT);
    return kick_worker(make_req(FG_FETCH_CHATS));
}

static bool kick_fetch_msgs()
{
    if (s_current_chat_id == 0) return false;
    if (s_msgs.empty()) set_status("Loading...", UI_COLOR_ACCENT);
    return kick_worker(make_req(FG_FETCH_MSGS));
}

static void kick_send(const char *text)
{
    if (s_current_chat_id == 0 || !text || !*text) return;
    if (s_bg_task != nullptr) {
        // Worker busy with a poll fetch — queue the send so the keystroke is
        // never dropped; the drain launches it as soon as the slot frees.
        s_pending_send = text;
        s_pending_send_chat = s_current_chat_id;
    } else {
        FgReq *r = make_req(FG_SEND);
        r->send_text = text;
        kick_worker(r);
    }
    set_status("Sending...", UI_COLOR_ACCENT);
}

// Cheap FNV-1a over the favorite-filtered chats (id, unread, title) so an
// identical poll can skip the rebuild. Favorites gate what render_chats shows,
// so they're part of the signature.
static uint32_t chats_signature(const std::vector<Chat> &v)
{
    uint32_t h = 2166136261u;
    for (const auto &c : v) {
        if (s_favorites.find(c.id) == s_favorites.end()) continue;
        for (int s = 0; s < 64; s += 8) h = (h ^ (uint32_t)((c.id >> s) & 0xff)) * 16777619u;
        for (int s = 0; s < 32; s += 8) h = (h ^ (((uint32_t)c.unread >> s) & 0xff)) * 16777619u;
        for (unsigned char ch : c.title) h = (h ^ ch) * 16777619u;
        h = (h ^ 0xffu) * 16777619u;   // record separator
    }
    return h;
}

// FNV-1a over the message list (id, out, from, text, media) so an unchanged
// history isn't torn down and re-pinned to the bottom every poll.
static uint32_t msgs_signature(const std::vector<Message> &v)
{
    uint32_t h = 2166136261u;
    for (const auto &m : v) {
        for (int s = 0; s < 32; s += 8) h = (h ^ (((uint32_t)m.id >> s) & 0xff)) * 16777619u;
        h = (h ^ (uint32_t)(m.out ? 1 : 0)) * 16777619u;
        for (unsigned char ch : m.from) h = (h ^ ch) * 16777619u;
        for (unsigned char ch : m.text) h = (h ^ ch) * 16777619u;
        for (unsigned char ch : m.media_type) h = (h ^ ch) * 16777619u;
        h = (h ^ 0xffu) * 16777619u;   // record separator
    }
    return h;
}

// UI-thread result applier. Runs inside lv_timer_handler() (so it already holds
// the instance mutex) and owns s_fg_result once it takes it.
static void tg_drain_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_fg_done) return;
    s_fg_done = false;
    FgResult *res = s_fg_result;
    s_fg_result = nullptr;
    if (!res) return;

    switch (res->op) {
        case FG_FETCH_CHATS:
            if (s_view == V_LIST) {
                if (res->ok) {
                    s_unread_total = res->unread_total;
                    // Only reseat s_chats + rebuild when the list actually
                    // changed. Skipping the move also keeps the live buttons'
                    // &s_chats[i] user-data valid.
                    uint32_t sig = chats_signature(res->chats);
                    if (!s_chats_sig_valid || sig != s_chats_sig) {
                        s_chats = std::move(res->chats);
                        s_chats_sig = sig;
                        s_chats_sig_valid = true;
                        render_chats();
                    }
                }
                set_status(res->status.c_str(), res->status_color);
            }
            break;
        case FG_FETCH_MSGS:
            if (s_view == V_CHAT && res->chat_id == s_current_chat_id) {
                // Skip the re-render while the user is scrolling history (it
                // would yank scroll to the bottom), and skip it when the
                // history is byte-identical to what's already on screen. Still
                // record any mark-read the worker performed either way.
                if (res->ok && !s_msgs_scroll_mode) {
                    uint32_t sig = msgs_signature(res->msgs);
                    if (!s_msgs_sig_valid || sig != s_msgs_sig) {
                        s_msgs = std::move(res->msgs);
                        s_msgs_sig = sig;
                        s_msgs_sig_valid = true;
                        render_msgs();
                    }
                }
                if (res->marked_to > s_last_marked_msg_id)
                    s_last_marked_msg_id = res->marked_to;
                set_status(res->status.c_str(), res->status_color);
            }
            break;
        case FG_SEND:
            if (s_view == V_CHAT && res->chat_id == s_current_chat_id) {
                set_status(res->status.c_str(), res->status_color);
                // Pull the just-sent line into view without waiting a poll.
                if (res->ok) kick_fetch_msgs();
            }
            break;
    }
    delete res;

    // Flush a send that was queued while the worker was busy (same chat only).
    if (!s_pending_send.empty()) {
        if (s_bg_task == nullptr && s_view == V_CHAT &&
            s_pending_send_chat == s_current_chat_id) {
            FgReq *r = make_req(FG_SEND);
            r->send_text.swap(s_pending_send);   // moves out & clears the slot
            s_pending_send_chat = 0;
            kick_worker(r);
        } else if (s_view != V_CHAT || s_pending_send_chat != s_current_chat_id) {
            s_pending_send.clear();               // chat changed → drop the draft
            s_pending_send_chat = 0;
        }
    }
}
#else
static void kick_send(const char *) {}
#endif

// --- polling --------------------------------------------------------------

static void stop_timer()
{
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
}

#ifdef ARDUINO
static void poll_tick(lv_timer_t *t)
{
    if (!configured()) return;
    if (!internet_available()) {
        set_status("No internet", UI_COLOR_MUTED);
        return;
    }
    // Don't churn the widget tree while the composer (or any textarea) is
    // focused: a re-render re-pins scroll and fights the caret. The fetch
    // itself no longer blocks the LVGL thread (it runs on the worker), but the
    // re-render on drain still would, so keep the pause.
    if (core::isTextInputFocused()) return;

    if (s_bg_task != nullptr) return;   // a fetch/send is already in flight

    bool kicked = false;
    switch (s_view) {
        case V_LIST:
            kicked = kick_fetch_chats();
            break;
        case V_CHAT:
            // Skip while the user is scrolling the history — a refetch
            // re-renders and pins scroll to the bottom, fighting them.
            if (s_msgs_scroll_mode) break;
            kicked = kick_fetch_msgs();
            break;
        default: break;
    }
    // Restore the normal per-view cadence once a fetch actually launches (an
    // earlier overlap with the notifier may have dropped us to a 1 s retry).
    if (kicked)
        lv_timer_set_period(t, s_view == V_CHAT ? TG_CHAT_POLL_MS : TG_LIST_POLL_MS);
}

static void start_timer(uint32_t period_ms)
{
    stop_timer();
    s_timer = lv_timer_create(poll_tick, period_ms, nullptr);
}
#endif

// --- view switching -------------------------------------------------------

static void back_to_menu_cb(lv_event_t *) { menu_show(); }
static void back_to_list_cb(lv_event_t *) { show_chat_list(); }

// --- inline composer ------------------------------------------------------

// Collapsed when not focused so the messages area takes the whole view; the
// user rotates onto it to expand and type. Same pattern as ui_tasks.cpp.
static void set_input_collapsed(lv_obj_t *ta, bool collapsed)
{
    if (!ta) return;
    if (collapsed) {
        if (s_input_expanded_h == 0) {
            lv_obj_update_layout(ta);
            s_input_expanded_h = lv_obj_get_height(ta);
        }
        lv_obj_set_height(ta, 0);
        lv_obj_set_style_pad_ver(ta, 0, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_opa(ta, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_set_height(ta,
            s_input_expanded_h > 0 ? s_input_expanded_h : LV_SIZE_CONTENT);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_PAD_TOP, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_PAD_BOTTOM, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_BORDER_WIDTH, 0);
        lv_obj_remove_local_style_prop(ta, LV_STYLE_OPA, 0);
    }
}

static void input_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(ta);

    if (code == LV_EVENT_FOCUSED) {
        set_input_collapsed(ta, false);
        // The message holder shrinks when flex re-lays the column, but its
        // scroll_y stays put, so the newest messages fall off the bottom
        // and look hidden behind the composer. Re-pin to the bottom.
        if (s_msgs_holder) {
            lv_obj_update_layout(s_root);
            lv_obj_scroll_to_y(s_msgs_holder, LV_COORD_MAX, LV_ANIM_OFF);
        }
        // Stay out of visual editing mode — the physical keyboard still
        // delivers characters to the focused textarea, and rotating the
        // encoder should move focus rather than move the caret.
        if (g) lv_group_set_editing(g, false);
    } else if (code == LV_EVENT_DEFOCUSED) {
        set_input_collapsed(ta, true);
        if (s_msgs_holder) {
            lv_obj_update_layout(s_root);
            lv_obj_scroll_to_y(s_msgs_holder, LV_COORD_MAX, LV_ANIM_OFF);
        }
    } else if (code == LV_EVENT_CLICKED) {
        lv_group_focus_obj(ta);
        if (g) lv_group_set_editing(g, false);
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ENTER) {
            const char *txt = lv_textarea_get_text(ta);
            if (txt && *txt) {
                // Async send: hand off to the worker and clear the box
                // optimistically so the next message can be typed right away.
                // On success the drain kicks a refetch so the sent line
                // appears; on failure the status pill shows the error.
                kick_send(txt);
                lv_textarea_set_text(ta, "");
            }
            lv_event_stop_processing(e);
        } else if (key == LV_KEY_ESC) {
            // Route through the shared status-bar back button so teardown
            // matches what the hardware back key does.
            lv_obj_t *bb = core::System::getInstance().getBackButton();
            if (bb) lv_obj_send_event(bb, LV_EVENT_CLICKED, NULL);
            lv_event_stop_processing(e);
        }
    }
}

// --- message-history scroll mode ------------------------------------------

// Clicking the messages container toggles encoder "scroll mode": while
// editing=true the encoder rotation scrolls the list; another click drops
// back to the normal focus chain (back / input / etc.).
static void msgs_holder_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *holder = (lv_obj_t *)lv_event_get_current_target(e);
    lv_group_t *g = (lv_group_t *)lv_obj_get_group(holder);
    if (!g) return;

    if (code == LV_EVENT_FOCUSED) {
        // Thin accent border while focused so the user can tell the
        // container is selectable; click bumps it to UI_BORDER_W.
        lv_obj_set_style_border_color(holder, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(holder, 1, 0);
    } else if (code == LV_EVENT_CLICKED) {
        bool editing = lv_group_get_editing(g) &&
                       lv_group_get_focused(g) == holder;
        lv_group_set_editing(g, !editing);
        s_msgs_scroll_mode = !editing;
        // Thicker border in scroll mode so the mode change is obvious.
        lv_obj_set_style_border_color(holder, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(holder, !editing ? UI_BORDER_W : 1, 0);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (lv_group_get_editing(g)) lv_group_set_editing(g, false);
        s_msgs_scroll_mode = false;
        lv_obj_set_style_border_width(holder, 0, 0);
    } else if (code == LV_EVENT_KEY) {
        if (!lv_group_get_editing(g) ||
            lv_group_get_focused(g) != holder) return;
        if (ui_scroll_key_clamped(holder, lv_event_get_key(e), 40, LV_ANIM_ON))
            lv_event_stop_processing(e);
    }
}

static void common_page_prep()
{
    if (!s_root) return;
    lv_obj_clean(s_root);
    lv_obj_set_style_bg_color(s_root, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 4, 0);
    lv_obj_set_style_pad_row(s_root, 4, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    // Drop any stale widget refs from the previous page.
    s_list_holder = nullptr;
    s_msgs_holder = nullptr;
    s_status_label = nullptr;
    s_input_ta = nullptr;
    s_input_expanded_h = 0;
    // Fresh (empty) holders → force the next render regardless of signature.
    s_chats_sig_valid = false;
    s_msgs_sig_valid = false;
}

static lv_obj_t *make_header(const char *title)
{
    lv_obj_t *hdr = lv_obj_create(s_root);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 2, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(hdr);
    lv_label_set_text(lbl, title);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, get_telegram_font(), 0);
    lv_obj_set_flex_grow(lbl, 1);
    return hdr;
}

static void show_chat_list()
{
    s_view = V_LIST;
    s_current_chat_id = 0;
    stop_timer();
    common_page_prep();
    ui_show_back_button(back_to_menu_cb);

    // Zero top padding so the chat list sits flush under the status bar.
    lv_obj_set_style_pad_top(s_root, 0, 0);

    s_list_holder = lv_list_create(s_root);
    lv_obj_set_flex_grow(s_list_holder, 1);
    lv_obj_set_width(s_list_holder, lv_pct(100));
    lv_obj_set_style_bg_opa(s_list_holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_holder, 0, 0);
    lv_obj_set_style_pad_top(s_list_holder, 0, 0);

    // Floating status pill at bottom-right, overlaying the list.
    s_status_label = make_pill(s_root, "", LV_ALIGN_BOTTOM_RIGHT, -4, -4);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    if (!configured()) {
        set_status("Not configured — Settings → Telegram", UI_COLOR_MUTED);
        return;
    }
#ifdef ARDUINO
    if (!hw_get_wifi_connected()) {
        set_status("WiFi not connected", UI_COLOR_MUTED);
        return;
    }
    if (!internet_available()) {
        // The verdict may still be pending on the async probe — keep re-checking
        // so the list loads as soon as it flips, instead of dead-ending here.
        set_status("No internet", UI_COLOR_MUTED);
        start_timer(TG_INET_RETRY_MS);
        return;
    }
    // Kick the first fetch onto the worker. If the slot is momentarily busy
    // (the notifier just fired), retry fast until it frees; otherwise settle
    // into the normal poll cadence.
    bool kicked = kick_fetch_chats();
    start_timer(kicked ? TG_LIST_POLL_MS : 1000);
#else
    set_status("Not supported on emulator.", UI_COLOR_MUTED);
#endif
}

static void show_chat(long long id, const char *title)
{
    s_view = V_CHAT;
    s_current_chat_id = id;
    s_current_chat_title = title ? title : "";
    s_last_marked_msg_id = 0;   // fresh chat → re-arm the mark-read POST
    s_input_expanded_h = 0;
    s_msgs_scroll_mode = false;
    stop_timer();
    common_page_prep();
    // Zero top/row padding so the chat name sits flush under the status bar
    // and the history fills the rest of the screen tightly.
    lv_obj_set_style_pad_top(s_root, 0, 0);
    lv_obj_set_style_pad_row(s_root, 2, 0);
    // Single-favorite flow: the chat list is redundant (only one entry), so
    // back out straight to the home menu. With 0 or >1 favorites the list
    // still makes sense, so go back to it as usual.
    ui_show_back_button(s_favorites.size() == 1 ? back_to_menu_cb
                                                : back_to_list_cb);

    // Non-flex wrapper so the chat-title pill can float on top of the
    // history area instead of taking its own row.
    lv_obj_t *msgs_wrap = lv_obj_create(s_root);
    lv_obj_set_width(msgs_wrap, lv_pct(100));
    lv_obj_set_flex_grow(msgs_wrap, 1);
    lv_obj_set_style_bg_opa(msgs_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msgs_wrap, 0, 0);
    lv_obj_set_style_pad_all(msgs_wrap, 0, 0);
    lv_obj_clear_flag(msgs_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_msgs_holder = lv_obj_create(msgs_wrap);
    lv_obj_set_size(s_msgs_holder, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(s_msgs_holder, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_msgs_holder, 0, 0);
    lv_obj_set_style_border_color(s_msgs_holder, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_msgs_holder, UI_RADIUS, 0);
    lv_obj_set_style_pad_all(s_msgs_holder, 2, 0);
    lv_obj_set_style_pad_row(s_msgs_holder, 3, 0);
    lv_obj_set_flex_flow(s_msgs_holder, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_msgs_holder, LV_DIR_VER);
    lv_obj_add_flag(s_msgs_holder, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(lv_group_get_default(), s_msgs_holder);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_KEY, nullptr);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(s_msgs_holder, msgs_holder_event_cb, LV_EVENT_DEFOCUSED, nullptr);

    // Floating chat-title pill at the top-right of the history area.
    make_pill(msgs_wrap, ascii_safe(s_current_chat_title).c_str(),
              LV_ALIGN_TOP_RIGHT, -2, 2);

    // Status label floats at bottom-right of history too, same style, so
    // errors/send feedback don't steal a line of message area.
    s_status_label = make_pill(msgs_wrap, "", LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    s_input_ta = lv_textarea_create(s_root);
    lv_textarea_set_one_line(s_input_ta, true);
    lv_textarea_set_placeholder_text(s_input_ta, "Message...");
    lv_obj_set_width(s_input_ta, lv_pct(100));
    lv_obj_set_style_text_font(s_input_ta, get_telegram_font(), 0);
    lv_obj_set_style_text_color(s_input_ta, UI_COLOR_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_add_event_cb(s_input_ta, input_event_cb, LV_EVENT_ALL, nullptr);
    lv_group_add_obj(lv_group_get_default(), s_input_ta);
    // Force the expanded height to be measured before collapse so the first
    // focus animates into a real size rather than LV_SIZE_CONTENT.
    lv_obj_update_layout(s_input_ta);
    s_input_expanded_h = lv_obj_get_height(s_input_ta);
    set_input_collapsed(s_input_ta, true);

    s_msgs.clear();
#ifdef ARDUINO
    if (!internet_available()) {
        // Keep re-checking while the async probe settles; recover without a re-open.
        set_status("No internet", UI_COLOR_MUTED);
        start_timer(TG_INET_RETRY_MS);
        return;
    }
    bool kicked = kick_fetch_msgs();
    start_timer(kicked ? TG_CHAT_POLL_MS : 1000);
#endif
}

// --- not-configured placeholder -------------------------------------------

// When the bridge URL or bearer token are missing the app has nothing to do,
// so instead of the chat list we point the user at the Settings screen
// where the fields now live (Settings → Telegram).
static void show_not_configured()
{
    s_view = V_NOT_CONFIGURED;
    stop_timer();
    common_page_prep();
    ui_show_back_button(back_to_menu_cb);

    make_header("Telegram");

    lv_obj_t *msg = lv_label_create(s_root);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_label_set_text(msg,
        "Not configured.\n\n"
        "Open Settings \xC2\xBB Telegram to set the bridge URL "
        "and bearer token.");
    lv_obj_set_width(msg, lv_pct(100));
    lv_obj_set_style_text_color(msg, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(msg, get_telegram_font(), 0);
}

// --- App ------------------------------------------------------------------

// Single-favorite shortcut: skip the chat list and drop the user straight
// into their one favorited chat. If we never cached the title (favorite
// added by an older build), show a generic "Chat" title rather than block
// the UI on an HTTP lookup.
static void show_default_view()
{
    if (s_favorites.size() == 1) {
        long long id = *s_favorites.begin();
        std::string title;
        auto it = s_favorite_titles.find(id);
        if (it != s_favorite_titles.end() && !it->second.empty()) {
            title = it->second;
        } else {
            // Avoid blocking HTTP fetch on UI open. Fallback to generic title.
            title = "Chat";
        }
        show_chat(id, title.c_str());
        return;
    }
    show_chat_list();
}

#ifdef ARDUINO
static void on_unlocked(bool ok, void *)
{
    if (!ok) {
        // User cancelled or got the passphrase wrong — kick back to menu
        // rather than silently sit on a screen with no token.
        menu_show();
        return;
    }
    reload_config();
    if (configured()) {
        show_default_view();
    } else {
        show_not_configured();
    }
}
#endif

class TelegramApp : public core::App {
public:
    TelegramApp() : core::App("Telegram") {}

    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        s_root = parent;
        reload_config();

#ifdef ARDUINO
        // Snap the background notifier back to the fast poll cadence so that
        // when the app closes the first background tick fires promptly.
        // While the app is open tg_bg_tick() early-returns (s_view != V_NONE),
        // so this period governs only the cadence after the next onStop().
        if (s_bg_timer) {
            lv_timer_set_period(s_bg_timer, TG_BG_POLL_FAST_MS);
            s_bg_timer_period = TG_BG_POLL_FAST_MS;
        }

        // Drop any result a worker from a previous session published after we
        // tore down (no drain timer was live to collect it) so it can't flash
        // into this fresh view. onStop already bumped the epoch, so a still-
        // running worker will discard rather than publish a new one.
        s_fg_done = false;
        if (s_fg_result) { delete s_fg_result; s_fg_result = nullptr; }
        s_pending_send.clear();
        s_pending_send_chat = 0;

        // If the token is at rest encrypted but the session is locked,
        // trigger the notes unlock modal before showing any app content.
        // ui_passphrase_unlock() fires the callback immediately when crypto
        // is disabled or already unlocked, so the common case is free.
        if (s_auth_header.empty() && token_is_encrypted()) {
            common_page_prep();
            s_status_label = lv_label_create(s_root);
            lv_label_set_text(s_status_label, "Unlocking...");
            lv_obj_set_style_text_color(s_status_label, UI_COLOR_MUTED, 0);
            lv_obj_set_style_text_font(s_status_label, get_telegram_font(), 0);
            ui_show_back_button(back_to_menu_cb);
            ui_passphrase_unlock(on_unlocked, nullptr);
            return;
        }
#endif
        if (configured()) {
            show_default_view();
        } else {
            show_not_configured();
        }
    }

    void onStop() override {
        stop_timer();
#ifdef ARDUINO
        // §2.1 async teardown: stop draining and abandon any in-flight/pending
        // result. The worker itself is left to self-delete — it only writes
        // heap state under the instance mutex, never the LVGL tree, so it is
        // safe to outlive us; bumping the epoch makes its late result be
        // discarded instead of published into a freed view.
        s_fg_epoch++;
        if (s_drain_timer) { lv_timer_del(s_drain_timer); s_drain_timer = nullptr; }
        s_fg_done = false;
        if (s_fg_result) { delete s_fg_result; s_fg_result = nullptr; }
        s_pending_send.clear();
        s_pending_send_chat = 0;
#endif
        ui_hide_back_button();
        s_view = V_NONE;
        s_current_chat_id = 0;
        s_root = nullptr;
        s_status_label = nullptr;
        s_list_holder = nullptr;
        s_msgs_holder = nullptr;
        s_input_ta = nullptr;
        s_chats.clear();
        s_msgs.clear();
        // Wipe the plaintext bearer header; clear() keeps capacity, so we
        // overwrite the buffer before release.
        hal::secret_scrub(s_auth_header);
        s_base_url.clear();
        core::App::onStop();
    }
};

// --- background unread-count poll -----------------------------------------

// Cached copies of the two notification-channel toggles. fire_notifications
// runs on every unread burst (from the bg-poll task); reading NVS twice each
// time is pure overhead since the values only change from the settings setters
// below. -1 = unloaded; seeded lazily, kept in sync by tg_cfg_set_notif_*.
static int8_t s_notif_toast_cache = -1;
static int8_t s_notif_vib_cache   = -1;

#ifdef ARDUINO
static bool notif_toast_enabled()
{
    if (s_notif_toast_cache < 0)
        s_notif_toast_cache = load_bool_pref("notif_toast", true) ? 1 : 0;
    return s_notif_toast_cache != 0;
}
static bool notif_vib_enabled()
{
    if (s_notif_vib_cache < 0)
        s_notif_vib_cache = load_bool_pref("notif_vib", true) ? 1 : 0;
    return s_notif_vib_cache != 0;
}

// Fires enabled notifiers for `delta` new unread messages via the shared
// notification bus. The prefs still gate each channel independently — a
// user who wants a toast without a buzz (or vice versa) keeps that control.
// Callable from the bg-poll task; notify::post() is thread-safe.
static void fire_notifications(int delta)
{
    if (delta <= 0) return;
    bool toast = notif_toast_enabled();
    bool vib   = notif_vib_enabled();

    if (toast) {
        core::notify::Notification n;
        n.icon     = LV_SYMBOL_ENVELOPE;
        if (delta <= 1) {
            n.title = "New Telegram message";
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "%d new Telegram messages", delta);
            n.title = buf;
        }
        n.severity = core::notify::Severity::Info;
        n.haptic   = vib;   // bus buzzes once with the banner
        core::notify::post(std::move(n));
    } else if (vib) {
        // Toast disabled but haptic still on — fire directly. hw_feedback
        // no-ops when the global haptic toggle is off.
        hw_feedback();
    }
}

static void tg_bg_task(void *arg)
{
    (void)arg;
    // Worker-safe gate only: tg_bg_tick() already checked the cached internet
    // verdict on the UI thread, and internet_available() itself is UI-thread
    // only (it reads lv_tick and may spawn the probe task).
    std::string url, auth;
    std::vector<Chat> chats;
    int sum = 0;
    bool ok = hw_get_wifi_connected() &&
              load_bridge_auth(url, auth, nullptr) &&
              fetch_chats(url, auth, chats, &sum, nullptr);
    hal::secret_scrub(auth);

    if (ok) {
        core::ScopedInstanceLock lock;
        s_unread_total = sum;

        // Seed the baseline on the first successful poll so we don't fire on
        // messages that arrived before the device booted. After that, any
        // positive delta triggers the enabled notifiers once.
        if (s_last_notified_unread < 0) {
            s_last_notified_unread = sum;
        } else if (sum > s_last_notified_unread) {
            fire_notifications(sum - s_last_notified_unread);
            s_last_notified_unread = sum;
        } else if (sum < s_last_notified_unread) {
            // Chats read elsewhere — follow the count down so we re-arm cleanly.
            s_last_notified_unread = sum;
        }
    }

    s_bg_task = nullptr;
    vTaskDelete(NULL);
}

static void tg_bg_tick(lv_timer_t *t)
{
    if (s_view != V_NONE) return;               // app is open → its timer drives updates
    if (!internet_available()) return;
    // Skip while typing elsewhere: this tick runs an HTTPS fetch (~1s).
    if (core::isTextInputFocused()) return;

    // Adaptive cadence: poll every TG_BG_POLL_FAST_MS while the user has
    // interacted recently (LVGL inactive time < TG_BG_IDLE_MS = 3 min), and
    // back off to TG_BG_POLL_SLOW_MS (5 min) when idle.  lv_timer_set_period()
    // reuses the existing timer — no allocation or teardown.  onStart() snaps
    // back to the fast cadence immediately whenever the Telegram app is opened,
    // and this same logic re-applies on the next tick after the user interacts.
    // lv_timers freeze during fake sleep, so the awake window is exactly where
    // this matters and exactly where the user is most likely to be looking.
    uint32_t inactive   = lv_display_get_inactive_time(NULL);
    uint32_t want_period = (inactive < TG_BG_IDLE_MS) ? TG_BG_POLL_FAST_MS
                                                       : TG_BG_POLL_SLOW_MS;
    if (s_bg_timer_period != want_period) {
        lv_timer_set_period(t, want_period);
        s_bg_timer_period = want_period;
    }

    if (s_bg_task != nullptr) return;           // previous fetch still in flight

    // Launch background task to avoid blocking the LVGL timer thread
    xTaskCreate(tg_bg_task, "tg_bg", 8192, nullptr, 2, &s_bg_task);
}
#endif

} // namespace

namespace apps {
APP_FACTORY(make_telegram_app, TelegramApp)

int tg_get_unread_count() { return s_unread_total; }

// --- config helpers called from settings_telegram.cpp ---------------------
// Their only callers run inside the Settings app, after TelegramApp::onStop
// has reset s_view to V_NONE, so none of them needs to refresh an open view:
// onStart re-reads the config and re-renders from scratch.

std::string tg_cfg_get_url()
{
    return load_pref("url");
}

void tg_cfg_set_url(const char *url)
{
    save_pref("url", url ? url : "");
}

// Masked form for the Settings label — see hal::secret_mask_display.
std::string tg_cfg_get_token_display()
{
    return hal::secret_mask_display(TG_PREFS_NS, "token_enc");
}

bool tg_cfg_set_token(const char *tok, std::string *err)
{
    return save_token(tok ? tok : "", err);
}

bool tg_cfg_token_is_encrypted()
{
    return token_is_encrypted();
}

// Per-channel notification toggles. Stored in the same tgbridge NVS namespace
// so they travel with the rest of the Telegram config. Defaults are ON so
// the user is notified out of the box — the Settings subpage can turn them
// off individually.
bool tg_cfg_get_notif_vibrate() { return load_bool_pref("notif_vib",   true); }
void tg_cfg_set_notif_vibrate(bool on) { save_bool_pref("notif_vib",   on); s_notif_vib_cache   = on ? 1 : 0; }
bool tg_cfg_get_notif_banner()  { return load_bool_pref("notif_toast", true); }
void tg_cfg_set_notif_banner(bool on)  { save_bool_pref("notif_toast", on); s_notif_toast_cache = on ? 1 : 0; }

bool tg_cfg_is_favorite(long long id)
{
    if (!s_favorites_loaded) load_favorites();
    return s_favorites.find(id) != s_favorites.end();
}

void tg_cfg_set_favorite(long long id, const char *title, bool on)
{
    if (!s_favorites_loaded) load_favorites();
    bool changed = false;
    if (on) {
        changed = s_favorites.insert(id).second;
        if (title && *title) {
            auto &cur = s_favorite_titles[id];
            if (cur != title) { cur = title; changed = true; }
        }
    } else {
        changed = s_favorites.erase(id) > 0;
        if (s_favorite_titles.erase(id) > 0) changed = true;
    }
    if (changed) save_favorites();
}

bool tg_cfg_fetch_all_chats(std::vector<std::pair<long long, std::string>> &out,
                            std::string *err)
{
    out.clear();
#ifdef ARDUINO
    // Runs on the Settings favourites worker, so only worker-safe checks here
    // (internet_available() is UI-thread only).
    std::string url, auth;
    if (!load_bridge_auth(url, auth, err)) return false;
    if (!hw_get_wifi_connected()) {
        hal::secret_scrub(auth);
        if (err) *err = "WiFi not connected";
        return false;
    }
    std::vector<Chat> chats;
    bool ok = fetch_chats(url, auth, chats, nullptr, err);
    hal::secret_scrub(auth);
    if (!ok) return false;
    out.reserve(chats.size());
    for (auto &c : chats) {
        if (c.id != 0) out.emplace_back(c.id, std::move(c.title));
    }
    return true;
#else
    (void)out;
    if (err) *err = "Not supported on emulator.";
    return false;
#endif
}

void tg_begin_background_poll() {
#ifdef ARDUINO
    if (s_bg_timer) return;
    // Start at the fast cadence (TG_BG_POLL_FAST_MS = 60 s).  tg_bg_tick()
    // backs off to TG_BG_POLL_SLOW_MS (5 min) once the LVGL inactive time
    // exceeds TG_BG_IDLE_MS (3 min), and snaps back to fast whenever the
    // user interacts or the Telegram app is opened (onStart).
    s_bg_timer_period = TG_BG_POLL_FAST_MS;
    s_bg_timer = lv_timer_create(tg_bg_tick, TG_BG_POLL_FAST_MS, nullptr);
#endif
}

} // namespace apps
