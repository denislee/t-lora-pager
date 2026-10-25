/**
 * @file      settings_telegram.cpp
 * @brief     Settings » Telegram + » Favorites subpages. Extracted from
 *            ui_settings.cpp; see settings_internal.h for the cross-TU
 *            contract.
 *
 * The actual storage lives in ui_telegram.cpp (NVS + optional AES wrap via
 * notes crypto). This file is just the UI over `apps::tg_cfg_*`.
 */
#include "../ui_define.h"
#include "../core/one_shot_job.h"
#include "app_registry.h"
#include "settings_internal.h"

namespace telegram_cfg {

static lv_obj_t *g_url_label      = nullptr;
static lv_obj_t *g_tok_label      = nullptr;
static lv_obj_t *g_note_label     = nullptr;
static lv_obj_t *g_fav_sub_page   = nullptr;

// Kept for the Favorites subpage — needs the parent menu object to call
// lv_menu_set_page from the button callback.
static lv_obj_t *g_parent_menu    = nullptr;

// The masked token needs a ~2 s PBKDF2 decrypt the first time per session,
// so refresh_labels starts it on a worker and the label is filled in when
// the result lands (P7.3).
static core::OneShotJob<std::string> g_mask_job;

static void fav_async_teardown();   // defined with the async block below

void reset_state()
{
    // Detach the token-label job before the widgets it writes into go away.
    g_mask_job.teardown();
    g_url_label = nullptr;
    g_tok_label = nullptr;
    g_note_label = nullptr;
    g_fav_sub_page = nullptr;
    g_parent_menu = nullptr;
    fav_async_teardown();
}

static void build_favorites_subpage(lv_obj_t *menu, lv_obj_t *sub_page);

// Fills the token/note labels from an already-resolved display string, so
// the cheap peek path and the worker's decrypt result share one form.
static void apply_token_labels(const std::string &display)
{
    settings_refresh_token_labels(
        g_tok_label, g_note_label,
        display, apps::tg_cfg_token_is_encrypted(),
        "Enable Notes encryption (Settings » Notes Security) "
        "before saving the bearer — the token is persisted "
        "only AES-256 wrapped, never plaintext.",
        "Token is AES-256 wrapped with your notes passphrase.");
}

static void mask_done(std::string &display)
{
    if (g_tok_label) apply_token_labels(display);
}

static void refresh_labels()
{
    if (g_url_label) {
        std::string url = apps::tg_cfg_get_url();
        lv_label_set_text_fmt(g_url_label, "URL: %s",
                              url.empty() ? "(not set)" : url.c_str());
    }
    // The masked token needs a ~2 s PBKDF2 decrypt the first time per
    // session, so it runs on a worker and the label is filled in when it
    // lands (P7.3).
    std::string display;
    if (apps::tg_cfg_peek_token_display(display)) {
        apply_token_labels(display);
    } else {
        apply_token_labels("********");
        g_mask_job.on_done(mask_done);
        g_mask_job.start([] {
            return new std::string(apps::tg_cfg_get_token_display());
        }, 8192, "tg_mask", 1);
    }
}

static void set_url_cb(const char *text, void *)
{
    if (!text) return;
    apps::tg_cfg_set_url(text);
    refresh_labels();
}

static void set_token_cb(const char *text, void *)
{
    if (!text) return;
    std::string err;
    if (!apps::tg_cfg_set_token(text, &err)) {
        ui_msg_pop_up("Token rejected",
                      err.empty() ? "Save failed." : err.c_str());
    }
    refresh_labels();
}

static void btn_set_url_cb(lv_event_t *)
{
    std::string current = apps::tg_cfg_get_url();
    ui_text_prompt("Bridge URL", "https://tg.example.com",
                   current.c_str(), set_url_cb, nullptr);
}

static void btn_open_favorites_cb(lv_event_t *)
{
    if (!g_parent_menu || !g_fav_sub_page) return;
    lv_menu_set_page(g_parent_menu, g_fav_sub_page);
}

static void btn_set_token_cb(lv_event_t *)
{
    // Intentionally don't pre-fill the token so we never read plaintext out
    // of NVS just to build a prompt — ui_text_prompt would echo it on-screen.
    ui_text_prompt("Bearer token", "from config.yaml on the bridge",
                   "", set_token_cb, nullptr);
}

// Notification toggles. create_toggle_btn_row flips the On/Off label.
static void notif_vibrate_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    apps::tg_cfg_set_notif_vibrate(en);
}

static void notif_banner_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    apps::tg_cfg_set_notif_banner(en);
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    g_parent_menu = menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(status, 2, 0);

    g_url_label = settings_muted_label(status);

    g_tok_label = settings_muted_label(status);

    settings_button_row(sub_page, LV_SYMBOL_WIFI, "Set URL", btn_set_url_cb);

    settings_button_row(sub_page, LV_SYMBOL_KEYBOARD, "Set token", btn_set_token_cb);

    // Favorites: clicking opens a nested subpage that fetches the full chat
    // list from the bridge and lets the user star/unstar each chat. The
    // Telegram app's listing renders only the starred ones. Match the
    // Set URL / Set token rows exactly — create_button (real cb) gives us the
    // same chevron bubble, and the cb triggers navigation via
    // lv_menu_set_page instead of relying on lv_menu_set_load_page_event
    // (which doesn't play well with a child btn inside the menu_cont).
    g_fav_sub_page = lv_menu_page_create(menu, NULL);
    lv_obj_set_user_data(g_fav_sub_page, (void*)build_favorites_subpage);
    settings_button_row(sub_page, LV_SYMBOL_EYE_OPEN, "Favorites", btn_open_favorites_cb);

    // Per-channel notification toggles. The background poll (running even
    // when the Telegram app is closed) dispatches to these when it sees the
    // total unread count go up. Haptic respects the global Haptic toggle in
    // Connectivity — if that's off, enabling "Vibrate" here still won't
    // buzz.
    create_toggle_btn_row(sub_page, "Vibrate on new msg",
                          apps::tg_cfg_get_notif_vibrate(), notif_vibrate_cb);
    create_toggle_btn_row(sub_page, "Banner on new msg",
                          apps::tg_cfg_get_notif_banner(), notif_banner_cb);

#ifdef ARDUINO
    lv_obj_t *note_row = lv_menu_cont_create(sub_page);
    g_note_label = settings_muted_label(note_row);
#endif

    // refresh_labels never decrypts on this thread (P7.3), so fill the labels now.
    refresh_labels();
}

// Per-button owned (id, title) — freed via LV_EVENT_DELETE. Title is kept
// so the toggle callback can push it through to tg_cfg_set_favorite, which
// caches it for the single-favorite shortcut in the Telegram app.
struct FavToggleCtx { long long id; std::string title; };

static void fav_id_delete_cb(lv_event_t *e)
{
    auto *ctx = (FavToggleCtx *)lv_event_get_user_data(e);
    delete ctx;
}

static void fav_toggle_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    auto *ctx = (FavToggleCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    bool checked = lv_obj_has_state(btn, LV_STATE_CHECKED);
    apps::tg_cfg_set_favorite(ctx->id, ctx->title.c_str(), checked);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (label) lv_label_set_text(label, checked ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
}

// Build the per-chat favorite-toggle rows into `page`.
static void fav_build_rows(lv_obj_t *page,
                           const std::vector<std::pair<long long, std::string>> &chats)
{
    for (auto &c : chats) {
        long long id = c.first;
        const std::string &title = c.second;

        lv_obj_t *row = create_text(page, title.c_str());

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_FG, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_BG, LV_STATE_CHECKED);
        lv_obj_set_size(btn, 44, 28);
        lv_obj_set_flex_grow(btn, 0);

        bool starred = apps::tg_cfg_is_favorite(id);
        if (starred) lv_obj_add_state(btn, LV_STATE_CHECKED);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, starred ? LV_SYMBOL_OK : LV_SYMBOL_PLUS);
        lv_obj_center(label);
        lv_obj_set_user_data(btn, label);

        auto *ctx = new FavToggleCtx{id, title};
        lv_obj_add_event_cb(btn, fav_toggle_cb, LV_EVENT_VALUE_CHANGED, ctx);
        lv_obj_add_event_cb(btn, fav_id_delete_cb, LV_EVENT_DELETE, ctx);
        register_subpage_group_obj(page, btn);
    }
}

// §2.17: the chat-list fetch is a blocking HTTPS GET (or a phone round trip);
// running it inline froze the LVGL thread (dead back button) for the request
// duration. A core::OneShotJob worker runs the fetch and hands a heap result
// back to the UI thread. The worker only touches its own heap result (never the
// widget tree), so it is safe to outlive the subpage; fav_async_teardown()
// detaches it before the menu objects are freed, so a late result never
// renders into a torn-down page.
struct FavFetchResult {
    std::vector<std::pair<long long, std::string>> chats;
    bool ok = false;
    std::string err;
};
static lv_obj_t *g_fav_status_lbl = nullptr;  // updated by fav_fetch_done
static lv_obj_t *g_fav_list_page  = nullptr;  // rows appended here

static void fav_fetch_done(FavFetchResult &res)
{
    if (!res.ok) {
        if (g_fav_status_lbl)
            lv_label_set_text_fmt(g_fav_status_lbl, "Fetch failed: %s", res.err.c_str());
    } else if (res.chats.empty()) {
        if (g_fav_status_lbl)
            lv_label_set_text(g_fav_status_lbl, "Bridge returned no chats.");
    } else {
        if (g_fav_status_lbl)
            lv_label_set_text_fmt(g_fav_status_lbl,
                "%u chat(s) — tap to toggle favorite.", (unsigned)res.chats.size());
        if (g_fav_list_page) {
            fav_build_rows(g_fav_list_page, res.chats);
            // The page's focus group was built when it opened, before these
            // rows existed: rebuild it so the encoder/keyboard reach them
            // (only while the page is still the one on screen).
            if (g_parent_menu &&
                lv_menu_get_cur_main_page(g_parent_menu) == g_fav_list_page) {
                activate_subpage_group(g_fav_list_page);
            }
        }
    }
}

static core::OneShotJob<FavFetchResult> g_fav_job(fav_fetch_done);

static void fav_async_teardown()
{
    // Detach the job first: its delivery (and only it) touches the menu
    // widgets, which ui_sys_exit frees right before calling reset_state. A
    // worker still running has its result freed on arrival.
    g_fav_job.teardown();
    g_fav_status_lbl = nullptr;
    g_fav_list_page  = nullptr;
}

static void build_favorites_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *status_row = lv_menu_cont_create(sub_page);
    lv_obj_t *status_lbl = settings_muted_label(status_row);
    lv_label_set_text(status_lbl, "Loading chats...");

    // Fetch off-thread; fav_fetch_done fills in rows/errors on completion.
    g_fav_status_lbl = status_lbl;
    g_fav_list_page  = sub_page;
    // A prior worker still in flight is re-attached, not duplicated.
    if (g_fav_job.start([]() {
            FavFetchResult *res = new FavFetchResult();
            res->ok = apps::tg_cfg_fetch_all_chats(res->chats, &res->err);
            return res;
        }, 8192, "tg_favs", 2) == core::OneShotJob<FavFetchResult>::Start::Failed) {
        lv_label_set_text(status_lbl, "task spawn fail");
    }
}

} // namespace telegram_cfg
