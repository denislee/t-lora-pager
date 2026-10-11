/**
 * @file      ui_lock.cpp
 * @brief     Modal passphrase prompt for the notes encryption feature.
 *
 * Provides an async overlay with a password-mode textarea and OK/Cancel
 * buttons. Callers supply a callback; the prompt fires it with either the
 * typed passphrase (or true/false for the verify-unlock variant) once the
 * user dismisses the modal. Only one prompt at a time.
 */
#include "ui_define.h"
#include "hal/notes_crypto.h"

#include <cstring>
#include <memory>
#include <string>

namespace {

struct LockCtx {
    ui_overlay_t ov        = {};   // no back button: ov.prev_back_cb stays NULL
    lv_obj_t *ta1          = nullptr;
    lv_obj_t *ta2          = nullptr;
    lv_obj_t *err_label    = nullptr;
    bool confirm           = false;
    bool verify_unlock     = false;
    bool password          = true;
    bool allow_empty       = false;
    ui_passphrase_result_cb cb_pw     = nullptr;
    ui_passphrase_unlock_cb cb_unlock = nullptr;
    void *ud               = nullptr;
};

/* Single-instance guard — the caller is expected to wait for one modal
 * before opening another. */
static std::unique_ptr<LockCtx> g_ctx;

static void set_error(LockCtx *ctx, const char *msg)
{
    if (!ctx || !ctx->err_label) return;
    /* Leave the label always visible — it has a reserved min-height so
     * toggling its text on and off doesn't reflow the rest of the dialog. */
    lv_label_set_text(ctx->err_label, msg ? msg : "");
}

static void tear_down(LockCtx *ctx)
{
    if (g_ctx.get() != ctx) return;
    /* Move ownership into a local so g_ctx is null while LVGL teardown
     * fires deletion events — they re-enter our callbacks and the null
     * g_ctx check guards against double-fire. */
    std::unique_ptr<LockCtx> owned = std::move(g_ctx);
    ui_overlay_close(&owned->ov);
}

static void cancel_event_cb(lv_event_t *e)
{
    LockCtx *ctx = (LockCtx *)lv_event_get_user_data(e);
    if (!ctx) return;
    ui_passphrase_result_cb cb_pw  = ctx->cb_pw;
    ui_passphrase_unlock_cb cb_un  = ctx->cb_unlock;
    void *ud = ctx->ud;
    tear_down(ctx);
    if (cb_pw) cb_pw(nullptr, ud);
    if (cb_un) cb_un(false, ud);
}

static void ok_event_cb(lv_event_t *e)
{
    LockCtx *ctx = (LockCtx *)lv_event_get_user_data(e);
    if (!ctx) return;

    const char *p1 = lv_textarea_get_text(ctx->ta1);
    if ((!p1 || !*p1) && !ctx->allow_empty) {
        set_error(ctx, ctx->password ? "Passphrase can't be empty."
                                     : "Value can't be empty.");
        return;
    }
    if (!p1) p1 = "";
    if (ctx->confirm) {
        const char *p2 = ctx->ta2 ? lv_textarea_get_text(ctx->ta2) : "";
        if (!p2 || strcmp(p1, p2) != 0) {
            set_error(ctx, "Passphrases don't match.");
            return;
        }
    }

    if (ctx->verify_unlock) {
        if (!notes_crypto_unlock(p1)) {
            set_error(ctx, "Wrong passphrase.");
            /* Clear the field so the user can retype rather than hand-delete. */
            lv_textarea_set_text(ctx->ta1, "");
            return;
        }
        ui_passphrase_unlock_cb cb = ctx->cb_unlock;
        void *ud = ctx->ud;
        tear_down(ctx);
        if (cb) cb(true, ud);
        return;
    }

    /* Copy the passphrase before tearing down — the textarea memory is gone
     * after lv_obj_del. */
    std::string pw(p1);
    ui_passphrase_result_cb cb = ctx->cb_pw;
    void *ud = ctx->ud;
    tear_down(ctx);
    if (cb) cb(pw.c_str(), ud);
}

/* Physical-keyboard Enter on a focused textarea submits the form rather
 * than inserting a newline. Encoder ENTER keeps its default (toggle edit). */
static void ta_key_event_cb(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_ENTER) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) return;
    lv_event_stop_processing(e);
    ok_event_cb(e);
}

static void build(const char *title, const char *subtitle,
                  bool confirm, bool verify_unlock,
                  ui_passphrase_result_cb cb_pw,
                  ui_passphrase_unlock_cb cb_unlock,
                  void *ud,
                  bool password = true,
                  const char *initial = nullptr,
                  bool allow_empty = false,
                  bool hide_buttons = false)
{
    if (g_ctx) {
        /* Already showing a prompt; drop the new request. */
        if (cb_pw)     cb_pw(nullptr, ud);
        if (cb_unlock) cb_unlock(false, ud);
        return;
    }

    g_ctx = std::make_unique<LockCtx>();
    LockCtx *ctx = g_ctx.get();
    ctx->confirm       = confirm;
    ctx->verify_unlock = verify_unlock;
    ctx->password      = password;
    ctx->allow_empty   = allow_empty;
    ctx->cb_pw         = cb_pw;
    ctx->cb_unlock     = cb_unlock;
    ctx->ud            = ud;

    /* No back-button takeover: the modal is dismissed by its own buttons
     * (or, for the device lock, only by the right passphrase). */
    lv_obj_t *overlay = ui_overlay_open(&ctx->ov, /*back_cb=*/nullptr, /*wrap=*/true);

    /* Dimmed full-screen backdrop instead of the below-status-bar panel —
     * the card below sits on top. Using a semi-transparent overlay keeps
     * whatever screen was visible as a faint background so the prompt feels
     * like a dialog, not a context switch. */
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(overlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(overlay, 6, 0);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_width(card, lv_pct(96));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
    lv_obj_set_style_radius(card, UI_RADIUS + 4, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Lock icon for password prompts — anchors the dialog visually. Skipped
     * for plain text prompts where it would be misleading. */
    if (password) {
        lv_obj_t *icon = lv_label_create(card);
        lv_label_set_text(icon, LV_SYMBOL_KEYBOARD);
        lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(icon, lv_theme_get_font_large(icon), 0);
    }

    if (title) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(t, lv_theme_get_font_large(t), 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (subtitle) {
        lv_obj_t *s = lv_label_create(card);
        lv_label_set_text(s, subtitle);
        lv_obj_set_style_text_color(s, UI_COLOR_MUTED, 0);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s, lv_pct(100));
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }

    auto style_ta = [](lv_obj_t *ta) {
        lv_obj_set_style_bg_color(ta, UI_COLOR_SURFACE, 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(ta, UI_COLOR_FG, 0);
        lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_radius(ta, UI_RADIUS, 0);
        lv_obj_set_style_border_color(ta, UI_COLOR_MUTED, 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_opa(ta, LV_OPA_60, 0);
        lv_obj_set_style_pad_all(ta, 8, 0);
        lv_obj_set_style_border_color(ta, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(ta, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_opa(ta, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUS_KEY);
    };

    ctx->ta1 = lv_textarea_create(card);
    lv_textarea_set_one_line(ctx->ta1, true);
    lv_textarea_set_password_mode(ctx->ta1, password);
    if (initial && *initial) {
        lv_textarea_set_text(ctx->ta1, initial);
    } else {
        lv_textarea_set_placeholder_text(
            ctx->ta1, password ? "passphrase" : "");
    }
    lv_obj_set_width(ctx->ta1, lv_pct(100));
    style_ta(ctx->ta1);
    lv_group_add_obj(ctx->ov.group, ctx->ta1);
    lv_obj_add_event_cb(ctx->ta1, ta_key_event_cb, LV_EVENT_KEY, ctx);

    if (confirm) {
        ctx->ta2 = lv_textarea_create(card);
        lv_textarea_set_one_line(ctx->ta2, true);
        lv_textarea_set_password_mode(ctx->ta2, true);
        lv_textarea_set_placeholder_text(ctx->ta2, "confirm");
        lv_obj_set_width(ctx->ta2, lv_pct(100));
        style_ta(ctx->ta2);
        lv_group_add_obj(ctx->ov.group, ctx->ta2);
        lv_obj_add_event_cb(ctx->ta2, ta_key_event_cb, LV_EVENT_KEY, ctx);
    }

    /* Always present, with a reserved min-height, so toggling the error text
     * on and off doesn't shift the buttons up and down. */
    ctx->err_label = lv_label_create(card);
    lv_label_set_text(ctx->err_label, "");
    lv_obj_set_style_text_color(ctx->err_label, lv_palette_main(LV_PALETTE_RED), 0);
    lv_label_set_long_mode(ctx->err_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->err_label, lv_pct(100));
    lv_obj_set_style_text_align(ctx->err_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_min_height(ctx->err_label, 18, 0);

    if (!hide_buttons) {
        lv_obj_t *btn_row = lv_obj_create(card);
        lv_obj_set_width(btn_row, lv_pct(100));
        lv_obj_set_height(btn_row, 40);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn_row, 0, 0);
        lv_obj_set_style_pad_all(btn_row, 0, 0);
        lv_obj_set_style_pad_column(btn_row, 8, 0);
        lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

        /* Shared modal-button look (ui_modal_btn_create), stretched to
         * split the row. */
        lv_obj_t *cancel_btn = ui_modal_btn_create(btn_row, LV_SYMBOL_CLOSE "  Cancel",
                                                   cancel_event_cb, ctx);
        lv_obj_set_flex_grow(cancel_btn, 1);
        lv_obj_set_height(cancel_btn, lv_pct(100));
        lv_group_add_obj(ctx->ov.group, cancel_btn);

        lv_obj_t *ok_btn = ui_modal_btn_create(btn_row, LV_SYMBOL_OK "  OK",
                                               ok_event_cb, ctx);
        lv_obj_set_flex_grow(ok_btn, 1);
        lv_obj_set_height(ok_btn, lv_pct(100));
        lv_group_add_obj(ctx->ov.group, ok_btn);
    }

    lv_group_focus_obj(ctx->ta1);
}

} /* namespace */

/* Public API (see ui/modals.h). */

bool ui_passphrase_is_active()
{
    return g_ctx != nullptr;
}

void ui_passphrase_unlock(ui_passphrase_unlock_cb cb, void *ud)
{
    if (!notes_crypto_is_enabled() || notes_crypto_is_unlocked()) {
        if (cb) cb(true, ud);
        return;
    }
    build("Unlock Notes",
          "Enter your passphrase",
          /*confirm=*/false, /*verify_unlock=*/true,
          /*cb_pw=*/nullptr, cb, ud);
}

void ui_passphrase_prompt(const char *title, const char *subtitle,
                          bool confirm,
                          ui_passphrase_result_cb cb, void *ud)
{
    build(title, subtitle, confirm, /*verify_unlock=*/false,
          cb, nullptr, ud);
}

void ui_text_prompt(const char *title, const char *subtitle,
                    const char *initial,
                    ui_passphrase_result_cb cb, void *ud)
{
    build(title, subtitle, /*confirm=*/false, /*verify_unlock=*/false,
          cb, nullptr, ud,
          /*password=*/false, initial, /*allow_empty=*/true);
}

/* Device-level lock: shows the unlock modal with no cancel button, so if
 * notes crypto is enabled and the session is locked nothing else in the UI
 * is reachable until the passphrase is entered. Safe to call unconditionally
 * — a no-op when crypto is disabled or already unlocked. */
void ui_device_lock_enforce()
{
    if (!notes_crypto_is_enabled() || notes_crypto_is_unlocked()) return;
    /* No title, no OK/Cancel buttons — the only way forward is to type the
     * passphrase and press Enter (handled by ta_key_event_cb). */
    build(/*title=*/nullptr,
          "Enter your passphrase",
          /*confirm=*/false, /*verify_unlock=*/true,
          /*cb_pw=*/nullptr, /*cb_unlock=*/nullptr, /*ud=*/nullptr,
          /*password=*/true, /*initial=*/nullptr, /*allow_empty=*/false,
          /*hide_buttons=*/true);
}
