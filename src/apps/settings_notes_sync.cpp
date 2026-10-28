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
#include "../core/one_shot_job.h"
#include "app_registry.h"
#include "settings_internal.h"

namespace notes_sync_cfg {

static lv_obj_t *g_repo_label  = nullptr;
static lv_obj_t *g_branch_label = nullptr;
static lv_obj_t *g_tok_label   = nullptr;
static lv_obj_t *g_note_label  = nullptr;

// The masked token needs a ~2 s PBKDF2 decrypt the first time per session,
// so refresh_labels starts it on a worker and the label is filled in when
// the result lands (P7.3).
static core::OneShotJob<std::string> g_mask_job;

void reset_state()
{
    // Detach the token-label job before the widgets it writes into go away.
    g_mask_job.teardown();
    g_repo_label = nullptr;
    g_branch_label = nullptr;
    g_tok_label = nullptr;
    g_note_label = nullptr;
}

// Fills the token/note labels from an already-resolved display string, so
// the cheap peek path and the worker's decrypt result share one form.
static void apply_token_labels(const std::string &display)
{
    settings_refresh_token_labels(
        g_tok_label, g_note_label,
        display, apps::nsync_cfg_token_is_encrypted(),
        "Enable Notes encryption (Settings » Notes Security) "
        "before saving a PAT — the token is persisted only "
        "AES-256 wrapped, never plaintext.",
        "Token is AES-256 wrapped with your notes passphrase. "
        "Private repos work — use a fine-grained PAT with "
        "Contents: read/write on just this repo.");
}

static void mask_done(std::string &display)
{
    if (g_tok_label) apply_token_labels(display);
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
    // The masked token needs a ~2 s PBKDF2 decrypt the first time per
    // session, so it runs on a worker and the label is filled in when it
    // lands (P7.3).
    std::string display;
    if (apps::nsync_cfg_peek_token_display(display)) {
        apply_token_labels(display);
    } else {
        apply_token_labels("********");
        g_mask_job.on_done(mask_done);
        g_mask_job.start([] {
            return new std::string(apps::nsync_cfg_get_token_display());
        }, 8192, "nsync_mask", 1);
    }
}

static void set_repo_cb(const char *text, void *)
{
    if (!text) return;
    std::string why;
    if (!settings_commit("nsync.repo", settings::Value::of_str(text), false, &why)) {
        ui_msg_pop_up("Repository rejected", why.c_str());
    }
    refresh_labels();
}

static void set_branch_cb(const char *text, void *)
{
    if (!text) return;
    std::string why;
    if (!settings_commit("nsync.branch", settings::Value::of_str(text), false, &why)) {
        ui_msg_pop_up("Branch rejected", why.c_str());
    }
    refresh_labels();
}

static void set_token_cb(const char *text, void *)
{
    if (!text) return;
    std::string err;
    if (!apps::nsync_cfg_set_token(text, &err)) {
        ui_msg_pop_up("Token rejected",
                      err.empty() ? "Save failed." : err.c_str());
    } else {
        settings::note_changed("nsync.token");
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

    g_repo_label = settings_muted_label(status);

    g_branch_label = settings_muted_label(status);

    g_tok_label = settings_muted_label(status);

    settings_button_row(sub_page, LV_SYMBOL_DIRECTORY, "Set repo", btn_set_repo_cb);

    settings_button_row(sub_page, LV_SYMBOL_SHUFFLE, "Set branch", btn_set_branch_cb);

    settings_button_row(sub_page, LV_SYMBOL_KEYBOARD, "Set token", btn_set_token_cb);

#ifdef ARDUINO
    lv_obj_t *note_row = lv_menu_cont_create(sub_page);
    g_note_label = settings_muted_label(note_row);
#endif

    // refresh_labels never decrypts on this thread (P7.3), so fill the labels now.
    refresh_labels();
}

} // namespace notes_sync_cfg
