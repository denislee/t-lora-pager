/**
 * @file      settings_notes_sync.cpp
 * @brief     Settings » Notes Sync subpage (GitHub repo + PAT). Extracted
 *            from ui_settings.cpp; see settings_internal.h for the
 *            cross-TU contract.
 *
 * Storage lives in ui_notes_sync.cpp (NVS "notesync" namespace; token is
 * AES-wrapped when notes crypto is enabled, same pattern as Telegram).
 * This file is just the UI over `apps::nsync_cfg_*`.
 */
#include "../ui_define.h"
#include "../hal/notes_crypto.h"
#include "app_registry.h"
#include "settings_internal.h"

namespace notes_sync_cfg {

static lv_obj_t *g_repo_label  = nullptr;
static lv_obj_t *g_branch_label = nullptr;
static lv_obj_t *g_tok_label   = nullptr;
static lv_obj_t *g_note_label  = nullptr;

void reset_state()
{
    g_repo_label = nullptr;
    g_branch_label = nullptr;
    g_tok_label = nullptr;
    g_note_label = nullptr;
}

static void refresh_labels()
{
    if (g_repo_label) {
        std::string v = apps::nsync_cfg_get_repo();
        lv_label_set_text_fmt(g_repo_label, "Repo: %s",
                              v.empty() ? "(not set)" : v.c_str());
    }
    if (g_branch_label) {
        std::string v = apps::nsync_cfg_get_branch();
        lv_label_set_text_fmt(g_branch_label, "Branch: %s", v.c_str());
    }
    settings_refresh_token_labels(
        g_tok_label, g_note_label,
        apps::nsync_cfg_get_token_display(), apps::nsync_cfg_token_is_encrypted(),
        "Enable Notes encryption (Settings » Notes Security) "
        "before saving a PAT — the token is persisted only "
        "AES-256 wrapped, never plaintext.",
        "Token is AES-256 wrapped with your notes passphrase. "
        "Private repos work — use a fine-grained PAT with "
        "Contents: read/write on just this repo.");
}

static void set_repo_cb(const char *text, void *)
{
    if (!text) return;
    apps::nsync_cfg_set_repo(text);
    refresh_labels();
}

static void set_branch_cb(const char *text, void *)
{
    if (!text) return;
    apps::nsync_cfg_set_branch(text);
    refresh_labels();
}

static void set_token_cb(const char *text, void *)
{
    if (!text) return;
    std::string err;
    if (!apps::nsync_cfg_set_token(text, &err)) {
        ui_msg_pop_up("Token rejected",
                      err.empty() ? "Save failed." : err.c_str());
    }
    refresh_labels();
}

static void btn_set_repo_cb(lv_event_t *)
{
    std::string current = apps::nsync_cfg_get_repo();
    ui_text_prompt("GitHub repo", "owner/repo",
                   current.c_str(), set_repo_cb, nullptr);
}

static void btn_set_branch_cb(lv_event_t *)
{
    std::string current = apps::nsync_cfg_get_branch();
    ui_text_prompt("Branch", "main",
                   current.c_str(), set_branch_cb, nullptr);
}

static void btn_set_token_cb(lv_event_t *)
{
    // No pre-fill so the PAT is never rendered back to the screen.
    ui_text_prompt("GitHub token",
                   "Personal access token (contents:write)",
                   "", set_token_cb, nullptr);
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(status, 2, 0);

    g_repo_label = lv_label_create(status);
    lv_label_set_long_mode(g_repo_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_repo_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_repo_label, UI_COLOR_MUTED, 0);

    g_branch_label = lv_label_create(status);
    lv_label_set_long_mode(g_branch_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_branch_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_branch_label, UI_COLOR_MUTED, 0);

    g_tok_label = lv_label_create(status);
    lv_label_set_long_mode(g_tok_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_tok_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_tok_label, UI_COLOR_MUTED, 0);

    lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_DIRECTORY,
                                 "Set repo", btn_set_repo_cb);
    register_subpage_group_obj(sub_page, b1);

    lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_SHUFFLE,
                                 "Set branch", btn_set_branch_cb);
    register_subpage_group_obj(sub_page, b2);

    lv_obj_t *b3 = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                 "Set token", btn_set_token_cb);
    register_subpage_group_obj(sub_page, b3);

#ifdef ARDUINO
    lv_obj_t *note_row = lv_menu_cont_create(sub_page);
    g_note_label = lv_label_create(note_row);
    lv_label_set_long_mode(g_note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_note_label, LV_PCT(100));
    lv_obj_set_style_text_color(g_note_label, UI_COLOR_MUTED, 0);
#endif

    // Placeholder text so the page paints with content during the menu
    // slide-in. The real refresh — NVS opens + PBKDF2/AES decrypt of the
    // token — is deferred to a one-shot timer so it runs after the
    // transition animation completes instead of stalling it.
    lv_label_set_text(g_repo_label,   "Repo: \xE2\x80\xA6");
    lv_label_set_text(g_branch_label, "Branch: \xE2\x80\xA6");
    lv_label_set_text(g_tok_label,    "Token: \xE2\x80\xA6");
#ifdef ARDUINO
    if (g_note_label) lv_label_set_text(g_note_label, "");
#endif
    lv_timer_t *t = lv_timer_create([](lv_timer_t *) { refresh_labels(); },
                                    1, nullptr);
    lv_timer_set_repeat_count(t, 1);
}

} // namespace notes_sync_cfg
