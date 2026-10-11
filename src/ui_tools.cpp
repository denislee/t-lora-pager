/**
 * @file      ui_tools.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 */
#include "ui_define.h"

static lv_group_t *msg_group = NULL;
static lv_group_t *prev_group;


const char *ui_battery_icon(int percent, bool is_charging)
{
    if (is_charging) return LV_SYMBOL_CHARGE;
    if (percent >= 88) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 63) return LV_SYMBOL_BATTERY_3;
    if (percent >= 38) return LV_SYMBOL_BATTERY_2;
    if (percent >= 13) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}


void destroy_msgbox(lv_obj_t *msgbox)
{
    lv_msgbox_close(msgbox);
    // lv_group_focus_freeze(msg_group, false);
    set_default_group(prev_group);
}

lv_obj_t *ui_popup_create(const char *title)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 12, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 8, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t *t = lv_label_create(overlay);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(t, lv_theme_get_font_large(t), 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        // Hotel: the title becomes a full-width Habbo title bar; BeOS: a
        // yellow window tab; the flat styles keep the accent-coloured label.
        if (ui_dress_title(t, t, 8) == UI_TITLE_BAR) {
            lv_obj_set_width(t, lv_pct(100));
            lv_obj_set_style_radius(t, 3, 0);
        }
    }
    return overlay;
}

void ui_popup_destroy(lv_obj_t *popup)
{
    if (popup) lv_obj_del(popup);
}

int32_t ui_status_bar_height(void)
{
    int32_t bar_h = lv_font_get_line_height(get_header_font()) + 8;
    return bar_h < 30 ? 30 : bar_h;
}

// ---------------------------------------------------------------------------
// Modal overlay + modal button. See ui/modals.h for contract.
// ---------------------------------------------------------------------------

lv_obj_t *ui_overlay_open(ui_overlay_t *o, lv_event_cb_t back_cb, bool wrap)
{
    o->prev_group = lv_group_get_default();
    o->prev_back_cb = back_cb ? ui_get_back_button_cb() : NULL;
    o->group = lv_group_create();
    lv_group_set_wrap(o->group, wrap);
    set_default_group(o->group);

    // ui_show_back_button adds the button to the current default group —
    // ours, set just above — so it stays keyboard-reachable as "close".
    if (back_cb) ui_show_back_button(back_cb);

    enable_keyboard();

    int32_t v_res = lv_display_get_vertical_resolution(NULL);
    if (v_res <= 0) v_res = 222;

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    o->overlay = overlay;
    lv_obj_set_size(overlay, lv_pct(100), v_res - ui_status_bar_height());
    lv_obj_align(overlay, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(overlay, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 8, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay, 6, 0);
    return overlay;
}

void ui_overlay_close(ui_overlay_t *o)
{
    if (o->overlay) lv_obj_del(o->overlay);
    if (o->group) lv_group_del(o->group);
    if (o->prev_group) set_default_group(o->prev_group);
    // Hand the back button back to the screen underneath. ui_show_back_button
    // re-adds it to the now-restored default group.
    if (o->prev_back_cb) ui_show_back_button(o->prev_back_cb);
    o->overlay = NULL;
    o->group = NULL;
    o->prev_group = NULL;
    o->prev_back_cb = NULL;
}

lv_obj_t *ui_modal_btn_create(lv_obj_t *parent, const char *text,
                              lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_radius(btn, UI_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_mix(UI_COLOR_ACCENT, UI_COLOR_SURFACE, LV_OPA_20),
                              LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, UI_COLOR_FG, 0);
    lv_obj_center(lbl);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

// ---------------------------------------------------------------------------
// Yes/No confirm. See ui/modals.h for contract.
// ---------------------------------------------------------------------------

namespace {
struct ConfirmCtx {
    ui_confirm_cb cb;
    void *ud;
    lv_obj_t *msgbox;
};
} // namespace

static void confirm_btn_cb(lv_event_t *e)
{
    ConfirmCtx *ctx = (ConfirmCtx *)lv_event_get_user_data(e);
    // Footer buttons are added in {"Yes", "No"} order.
    bool yes = lv_obj_get_index((lv_obj_t *)lv_event_get_target(e)) == 0;
    ConfirmCtx c = *ctx;
    delete ctx;
    destroy_msgbox(c.msgbox);
    if (c.cb) c.cb(yes, c.ud);
}

void ui_confirm(const char *title, const char *msg, ui_confirm_cb cb, void *ud)
{
    static const char *btns[] = {"Yes", "No", ""};
    ConfirmCtx *ctx = new ConfirmCtx{cb, ud, NULL};
    ctx->msgbox = create_msgbox(NULL, title, msg, btns, confirm_btn_cb, ctx);
}

// ---------------------------------------------------------------------------
// Unified loading / progress popup. See ui/modals.h for contract.
// ---------------------------------------------------------------------------

static lv_obj_t *_loading_make_spinner(lv_obj_t *parent)
{
    lv_obj_t *sp = lv_spinner_create(parent);
    lv_spinner_set_anim_params(sp, 1000, 200);
    lv_obj_set_size(sp, 48, 48);
    lv_obj_set_style_arc_color(sp, UI_COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
    return sp;
}

static lv_obj_t *_loading_make_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, lv_pct(80), 10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, UI_COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    return bar;
}

void ui_loading_open(ui_loading_t *l, const char *title, const char *detail)
{
    if (!l) return;
    l->overlay = ui_popup_create(title);
    // Indicator slot (index 1, just under the title). Replaced between
    // spinner/bar as set_progress() / set_indeterminate() is called.
    l->spinner = _loading_make_spinner(l->overlay);
    l->bar = nullptr;

    l->counts = lv_label_create(l->overlay);
    lv_label_set_text(l->counts, "");
    lv_obj_set_style_text_color(l->counts, UI_COLOR_FG, 0);
    lv_obj_set_style_text_align(l->counts, LV_TEXT_ALIGN_CENTER, 0);

    l->detail = lv_label_create(l->overlay);
    lv_label_set_text(l->detail, detail ? detail : "");
    lv_obj_set_style_text_color(l->detail, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_align(l->detail, LV_TEXT_ALIGN_CENTER, 0);
    // WRAP (rather than DOT) so callers can stack phase + filename on
    // separate lines with "\n" — journal scan, bulk syncs use that.
    lv_label_set_long_mode(l->detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l->detail, lv_pct(80));
    lv_obj_set_style_text_font(l->detail, get_small_font(), 0);

    lv_refr_now(NULL);
}

void ui_loading_set_indeterminate(ui_loading_t *l, const char *detail)
{
    if (!l || !l->overlay) return;
    if (l->bar) {
        lv_obj_del(l->bar);
        l->bar = nullptr;
    }
    if (!l->spinner) {
        l->spinner = _loading_make_spinner(l->overlay);
        lv_obj_move_to_index(l->spinner, 1);
    }
    if (l->counts) lv_label_set_text(l->counts, "");
    if (l->detail) lv_label_set_text(l->detail, detail ? detail : "");
}

void ui_loading_set_progress(ui_loading_t *l, int cur, int total, const char *detail)
{
    if (!l || !l->overlay) return;
    if (l->spinner) {
        lv_obj_del(l->spinner);
        l->spinner = nullptr;
    }
    if (!l->bar) {
        l->bar = _loading_make_bar(l->overlay);
        lv_obj_move_to_index(l->bar, 1);
    }
    int pct;
    if (total > 0) {
        pct = (int)((int64_t)cur * 100 / total);
    } else {
        pct = cur;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(l->bar, pct, LV_ANIM_OFF);

    if (l->counts) {
        char buf[48];
        if (total > 0) {
            snprintf(buf, sizeof(buf), "%d / %d  -  %d%%", cur, total, pct);
        } else {
            snprintf(buf, sizeof(buf), "%d%%", pct);
        }
        lv_label_set_text(l->counts, buf);
    }
    if (l->detail) lv_label_set_text(l->detail, detail ? detail : "");
}

void ui_loading_close(ui_loading_t *l)
{
    if (!l) return;
    if (l->overlay) ui_popup_destroy(l->overlay);
    l->overlay = nullptr;
    l->spinner = nullptr;
    l->bar = nullptr;
    l->counts = nullptr;
    l->detail = nullptr;
}

// ---------------------------------------------------------------------------
// Structured result popup. Reuses the create_msgbox pipeline but replaces
// the body with a flex column of "label : value" rows rendered at font_18
// so counts line up vertically. Falls back to plain text when no rows are
// supplied.
// ---------------------------------------------------------------------------

static lv_obj_t *s_result_msgbox = NULL;

static void _result_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_result_msgbox) {
        destroy_msgbox(s_result_msgbox);
        s_result_msgbox = NULL;
    }
}

void ui_result_show(const char *title, const char *subtitle,
                    const ui_summary_row_t *rows, size_t n_rows)
{
    if (s_result_msgbox) return;  // only one result modal at a time
    static const char *btns[] = {"OK", ""};

    // create_msgbox renders the msg_txt via lv_msgbox_add_text(). Pass an
    // empty string here and rebuild the body ourselves so rows can line up.
    s_result_msgbox = create_msgbox(lv_scr_act(), title, "", btns,
                                    _result_close_cb, NULL);
    if (!s_result_msgbox) return;

    lv_obj_t *content = lv_msgbox_get_content(s_result_msgbox);
    if (!content) return;

    // Strip the default text label lv_msgbox_add_text created (an empty
    // string label at the top) — we rebuild the body from scratch.
    uint32_t n = lv_obj_get_child_count(content);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_del(lv_obj_get_child(content, 0));
    }

    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(content, 6, 0);

    if (subtitle && subtitle[0]) {
        lv_obj_t *sub = lv_label_create(content);
        lv_label_set_text(sub, subtitle);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, lv_pct(100));
        lv_obj_set_style_text_color(sub, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_bottom(sub, 4, 0);
    }

    // Stats card: a single rounded container holding the rows. The card
    // sits inside the msgbox content with a muted accent outline so the
    // data feels visually grouped, not just a loose list.
    if (n_rows > 0) {
        lv_obj_t *card = lv_obj_create(content);
        lv_obj_remove_style_all(card);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_bg_color(card, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_10, 0);
        lv_obj_set_style_border_color(card, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
        lv_obj_set_style_radius(card, UI_RADIUS, 0);
        lv_obj_set_style_pad_hor(card, 12, 0);
        lv_obj_set_style_pad_ver(card, 8, 0);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        for (size_t i = 0; i < n_rows; i++) {
            lv_obj_t *row = lv_obj_create(card);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, rows[i].label ? rows[i].label : "");
            lv_obj_set_style_text_color(lbl, UI_COLOR_MUTED, 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_flex_grow(lbl, 1);

            // Value: right-aligned, accent-colored so numbers pop against
            // the muted labels.
            lv_obj_t *val = lv_label_create(row);
            lv_label_set_text(val, rows[i].value ? rows[i].value : "");
            lv_obj_set_style_text_color(val, UI_COLOR_ACCENT, 0);
            lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
        }
    }

    // lv_msgbox's SIZE_CHANGED handler only flips content->flex_grow to 1
    // when the msgbox height is NOT LV_SIZE_CONTENT. With SIZE_CONTENT +
    // max_height the msgbox clamps but children keep natural sizes, so tall
    // bodies push the footer (OK) below the visible area on the short
    // 480x222 pager. If we overflow max_height, switch to a fixed height so
    // content gets flex_grow=1, shrinks, and scrolls internally.
    lv_obj_update_layout(s_result_msgbox);
    int32_t natural_h = lv_obj_get_height(s_result_msgbox);
    int32_t screen_h =
        lv_display_get_vertical_resolution(lv_obj_get_display(s_result_msgbox));
    int32_t max_h = (screen_h * 85) / 100;
    if (natural_h > max_h) {
        lv_obj_set_height(s_result_msgbox, max_h);
    }
}

lv_obj_t *create_msgbox(lv_obj_t *parent, const char *title_txt,
                        const char *msg_txt, const char **btns,
                        lv_event_cb_t btns_event_cb, void *user_data)
{

    prev_group = lv_group_get_default();

    if (!msg_group) {
        msg_group = lv_group_create();
        lv_group_set_wrap(msg_group, true);
    }
    lv_group_remove_all_objs(msg_group);
    set_default_group(msg_group);

    lv_obj_t *msgbox;

    msgbox = lv_msgbox_create(lv_layer_top());

    // Sizing: width caps at 90% so short messages don't stretch; height
    // grows with content (min 30%, max 85%) so a one-liner doesn't render
    // inside a huge mostly-empty panel. The inner scroll takes over if the
    // content exceeds max_height.
    lv_obj_set_width(msgbox, lv_pct(90));
    lv_obj_set_height(msgbox, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(msgbox, lv_pct(85), 0);
    lv_obj_set_style_min_height(msgbox, lv_pct(30), 0);

    lv_obj_set_style_bg_color(msgbox, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(msgbox, LV_OPA_COVER, 0);
    // Accent frame on the flat styles, the chrome keyline on Hotel / BeOS
    // (BeOS: a flat 1px frame with its corners squared off).
    const ui_palette_t &pal = ui_palette();
    lv_obj_set_style_border_color(msgbox, pal.frame, 0);
    lv_obj_set_style_border_width(msgbox, pal.frame_w, 0);
    lv_obj_set_style_radius(msgbox, pal.frame_radius, 0);
    lv_obj_set_style_text_color(msgbox, UI_COLOR_FG, 0);
    lv_obj_set_style_pad_all(msgbox, 0, 0);
    lv_obj_set_style_shadow_width(msgbox, 18, 0);
    lv_obj_set_style_shadow_opa(msgbox, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(msgbox, lv_color_black(), 0);

    // Hotel keeps the header and dresses it as a Habbo title bar; BeOS
    // keeps it too and turns the title into a yellow window tab; every other
    // style hides it (popups no longer carry a title bar).
    lv_obj_t *hdr = lv_msgbox_get_header(msgbox);
    if (ui_style_has_chrome()) {
        // lv_msgbox_add_title lazily creates the header, so re-fetch it.
        lv_obj_t *title_lbl = lv_msgbox_add_title(msgbox,
            (title_txt && *title_txt) ? title_txt : " ");
        hdr = lv_msgbox_get_header(msgbox);
        if (ui_dress_title(hdr, title_lbl, 10) == UI_TITLE_TAB && hdr) {
            // The BeOS tab sits on a bare strip, inset from the frame edge.
            lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(hdr, 0, 0);
            lv_obj_set_style_pad_all(hdr, 0, 0);
            lv_obj_set_style_pad_left(hdr, 6, 0);
            lv_obj_set_style_pad_top(hdr, 0, 0);
        }
    } else if (hdr) {
        lv_obj_add_flag(hdr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(hdr, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_set_style_border_width(hdr, 0, 0);
    }

    lv_msgbox_add_text(msgbox, msg_txt);
    lv_obj_t *content = lv_msgbox_get_content(msgbox);
    if (content) {
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_set_style_pad_hor(content, 16, 0);
        lv_obj_set_style_pad_ver(content, 14, 0);
        lv_obj_set_style_pad_row(content, 6, 0);
        lv_obj_set_style_text_color(content, UI_COLOR_FG, 0);
        lv_obj_set_style_text_line_space(content, 3, 0);
    }

    // Button styling. Two styles: base (muted border) and focused/pressed
    // (accent fill). Applied per-button so lv_msgbox_add_footer_button's
    // theme defaults don't leak through. Rebuilt whenever the UI style
    // changes, since the colors below depend on the active palette.
    static lv_style_t msgbox_btn_base_style;
    static lv_style_t msgbox_btn_focus_style;
    static int msgbox_btn_style_for = -1;
    int style_key = (int)ui_style_get() * 2 + (ui_dark_mode_get() ? 1 : 0);
    if (msgbox_btn_style_for != style_key) {
        if (msgbox_btn_style_for >= 0) {
            lv_style_reset(&msgbox_btn_base_style);
            lv_style_reset(&msgbox_btn_focus_style);
        }
        lv_style_init(&msgbox_btn_base_style);
        lv_style_set_bg_color(&msgbox_btn_base_style, pal.btn_face);
        lv_style_set_bg_opa(&msgbox_btn_base_style, LV_OPA_COVER);
        lv_style_set_border_width(&msgbox_btn_base_style, pal.keyline_w);
        lv_style_set_border_color(&msgbox_btn_base_style, ui_color_outline());
        lv_style_set_text_color(&msgbox_btn_base_style, UI_COLOR_FG);
        lv_style_set_radius(&msgbox_btn_base_style, UI_RADIUS);
        lv_style_set_pad_hor(&msgbox_btn_base_style, 18);
        lv_style_set_pad_ver(&msgbox_btn_base_style, 2);
        lv_style_set_min_width(&msgbox_btn_base_style, 72);
        lv_style_set_height(&msgbox_btn_base_style, 28);
        lv_style_set_shadow_width(&msgbox_btn_base_style, 0);

        lv_style_init(&msgbox_btn_focus_style);
        lv_style_set_bg_color(&msgbox_btn_focus_style, UI_COLOR_ACCENT);
        lv_style_set_bg_opa(&msgbox_btn_focus_style, LV_OPA_COVER);
        lv_style_set_border_color(&msgbox_btn_focus_style, UI_COLOR_ACCENT);
        lv_style_set_border_width(&msgbox_btn_focus_style, 1);
        lv_style_set_text_color(&msgbox_btn_focus_style, UI_COLOR_ON_ACCENT);
        lv_style_set_radius(&msgbox_btn_focus_style, UI_RADIUS);
        msgbox_btn_style_for = style_key;
    }

    lv_obj_t *footer = lv_msgbox_get_footer(msgbox);
    if (footer) {
        lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(footer, 0, 0);
        lv_obj_set_style_pad_hor(footer, 14, 0);
        lv_obj_set_style_pad_top(footer, 4, 0);
        lv_obj_set_style_pad_bottom(footer, 14, 0);
        lv_obj_set_style_pad_column(footer, 10, 0);
        lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    uint32_t btn_cnt = 0;
    lv_obj_t *btn;
    lv_obj_t *first_btn = NULL;
    while (btns[btn_cnt] && btns[btn_cnt][0] != '\0') {
        btn = lv_msgbox_add_footer_button(msgbox, btns[btn_cnt]);
        lv_obj_add_style(btn, &msgbox_btn_base_style, 0);
        lv_obj_add_style(btn, &msgbox_btn_focus_style, LV_STATE_FOCUS_KEY);
        lv_obj_add_style(btn, &msgbox_btn_focus_style, LV_STATE_FOCUSED);
        lv_obj_add_style(btn, &msgbox_btn_focus_style, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, btns_event_cb, LV_EVENT_CLICKED, user_data);
        lv_group_add_obj(msg_group, btn);
        if (btn_cnt == 0) first_btn = btn;
        btn_cnt++;
    }
    if (first_btn) {
        lv_group_focus_obj(first_btn);
        lv_obj_add_state(first_btn, LV_STATE_FOCUS_KEY);
    }


    return msgbox;

}

// Mirror a row child's focus onto the row itself so the whole menu row
// highlights. Shared with the Settings subpages (see ui/widgets.h).
void toggle_child_focus_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * parent = lv_obj_get_parent(obj);
    if(code == LV_EVENT_FOCUSED) {
        lv_obj_add_state(parent, LV_STATE_FOCUSED);
        lv_obj_add_state(parent, LV_STATE_FOCUS_KEY);
    } else if(code == LV_EVENT_DEFOCUSED) {
        lv_obj_remove_state(parent, LV_STATE_FOCUSED);
        lv_obj_remove_state(parent, LV_STATE_FOCUS_KEY);
    }
}

bool ui_scroll_key_clamped(lv_obj_t *obj, uint32_t key, int32_t step,
                           lv_anim_enable_t anim)
{
    if (key == LV_KEY_UP || key == LV_KEY_LEFT) {
        int32_t room = lv_obj_get_scroll_y(obj);       // px above the view
        if (room < step) step = room;
        if (step > 0) lv_obj_scroll_by(obj, 0, step, anim);
        return true;
    }
    if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT) {
        int32_t room = lv_obj_get_scroll_bottom(obj);  // px below the view
        if (room < step) step = room;
        if (step > 0) lv_obj_scroll_by(obj, 0, -step, anim);
        return true;
    }
    return false;
}

lv_obj_t *ui_list_add_meta_row(lv_obj_t *list, const char *icon, const char *text,
                               const char *meta, const lv_font_t *meta_font)
{
    lv_obj_t *btn = lv_list_add_btn(list, icon, text);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *icon_obj = lv_obj_get_child(btn, 0);
    lv_obj_t *lbl      = lv_obj_get_child(btn, 1);
    if (icon_obj) lv_obj_set_width(icon_obj, 20);
    if (lbl) {
        lv_obj_set_width(lbl, 0);
        lv_obj_set_flex_grow(lbl, 1);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    }
    if (meta) {
        lv_obj_t *m = lv_label_create(btn);
        lv_label_set_text(m, meta);
        lv_obj_set_style_text_color(m, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_font(m, meta_font, 0);
        lv_obj_set_style_pad_left(m, 8, 0);
    }
    return btn;
}

// ---------------------------------------------------------------------------
// BLE volume button. See ui/widgets.h for contract.
// ---------------------------------------------------------------------------

namespace {
struct BleVolumeSpec {
    void (*refresh)(void);
    bool require_connected;
};
} // namespace

static void ble_volume_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    BleVolumeSpec *spec = (BleVolumeSpec *)lv_event_get_user_data(e);
    if (code == LV_EVENT_DELETE) {
        lv_free(spec);
        return;
    }
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_group_t *g = lv_obj_get_group(btn);
    if (!g) return;

    // Encoder press on a non-editable widget fires both LV_EVENT_KEY (ENTER)
    // and LV_EVENT_CLICKED. Handle the toggle in CLICKED only (works for
    // touch too) and ignore ENTER in the key branch to avoid double-toggle.
    if (code == LV_EVENT_CLICKED) {
        if (spec->require_connected && !hw_get_ble_kb_connected()) return;
        lv_group_set_editing(g, !lv_group_get_editing(g));
        hw_feedback();
        if (spec->refresh) spec->refresh();
    } else if (code == LV_EVENT_KEY) {
        uint32_t key = lv_event_get_key(e);
        if (key == LV_KEY_ENTER) return;
        if (!lv_group_get_editing(g)) return;
        if (spec->require_connected && !hw_get_ble_kb_connected()) return;
        // Scroll wheel: clockwise (NEXT/RIGHT/UP) decreases volume on the
        // LilyGo encoder — swap to match the physical scroll direction.
        if (key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_NEXT) {
            hw_feedback();
            hw_set_ble_key(MEDIA_VOLUME_DOWN);
            lv_event_stop_processing(e);
        } else if (key == LV_KEY_LEFT || key == LV_KEY_DOWN
                   || key == LV_KEY_PREV) {
            hw_feedback();
            hw_set_ble_key(MEDIA_VOLUME_UP);
            lv_event_stop_processing(e);
        }
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_group_set_editing(g, false);
        if (spec->refresh) spec->refresh();
    }
}

void ble_volume_btn_attach(lv_obj_t *btn, void (*refresh)(void), bool require_connected)
{
    BleVolumeSpec *spec = (BleVolumeSpec *)lv_malloc(sizeof(BleVolumeSpec));
    spec->refresh = refresh;
    spec->require_connected = require_connected;
    lv_obj_add_event_cb(btn, ble_volume_event_cb, LV_EVENT_CLICKED, spec);
    lv_obj_add_event_cb(btn, ble_volume_event_cb, LV_EVENT_KEY, spec);
    lv_obj_add_event_cb(btn, ble_volume_event_cb, LV_EVENT_DEFOCUSED, spec);
    lv_obj_add_event_cb(btn, ble_volume_event_cb, LV_EVENT_DELETE, spec);
}

// Menu row: optional icon image followed by a growing text label.
static lv_obj_t *create_row(lv_obj_t *parent, const char *icon, const char *txt)
{
    lv_obj_t *obj = lv_menu_cont_create(parent);

    if (icon) {
        lv_obj_t *img = lv_img_create(obj);
        lv_img_set_src(img, icon);
    }

    if (txt) {
        lv_obj_t *label = lv_label_create(obj);
        lv_label_set_text(label, txt);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);
    }

    return obj;
}

lv_obj_t *create_text(lv_obj_t *parent, const char *txt)
{
    return create_row(parent, NULL, txt);
}

lv_obj_t *create_slider(lv_obj_t *parent, const char *txt, int32_t min, int32_t max,
                        int32_t val, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_text(parent, txt);

    lv_obj_t *slider = lv_slider_create(obj);
    lv_obj_set_style_outline_width(slider, 0, 0);
    lv_obj_set_style_outline_width(slider, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(slider, 0, 0);
    lv_obj_set_style_border_width(slider, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(slider, 0, LV_STATE_FOCUSED);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, val, LV_ANIM_OFF);

    lv_obj_add_event_cb(slider, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(slider, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);

    if (cb != NULL) {
        lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }

    return slider;
}

lv_obj_t *create_button(lv_obj_t *parent, const char *icon, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_row(parent, icon, txt);
    lv_obj_t *btn = lv_btn_create(obj);

    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, 0);

    lv_obj_set_width(btn, 60);
    lv_obj_set_flex_grow(btn, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_RIGHT);
    lv_obj_center(label);

    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

lv_obj_t *create_dropdown(lv_obj_t *parent, const char *txt, const char *options, uint8_t default_sel, lv_event_cb_t cb)
{
    lv_obj_t *obj = create_text(parent, txt);
    lv_obj_t *dd = lv_dropdown_create(obj);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, default_sel);

    // The dropdown already shows its own focus highlight via the theme,
    // so don't propagate focus to the parent row — avoids a redundant
    // selection bubble around the whole row.

    if (cb) {
        lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    return dd;
}

lv_obj_t *create_floating_button(lv_event_cb_t event_cb, void* user_data)
{
    lv_obj_t *float_btn = lv_btn_create(lv_screen_active());
    lv_obj_set_size(float_btn, FLOAT_BUTTON_WIDTH, FLOAT_BUTTON_HEIGHT);
    lv_obj_add_flag(float_btn, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(float_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(float_btn, event_cb, LV_EVENT_CLICKED, user_data);
    lv_obj_set_style_radius(float_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_image_src(float_btn, LV_SYMBOL_LEFT, 0);
    lv_obj_set_style_text_font(float_btn, lv_theme_get_font_large(float_btn), 0);
    return float_btn;
}


lv_obj_t *create_menu(lv_obj_t *parent, lv_event_cb_t event_cb)
{
    lv_obj_t *menu = lv_menu_create(parent);
    lv_menu_set_mode_root_back_button(menu, LV_MENU_ROOT_BACK_BUTTON_DISABLED);
    if (event_cb) lv_obj_add_event_cb(menu, event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_center(menu);
    return menu;
}

#ifndef ARDUINO
lv_indev_t *lv_get_encoder_indev()
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            return indev;
        }
        indev = lv_indev_get_next(indev);
    }
    return NULL;
}


lv_indev_t *lv_get_keyboard_indev()
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            return indev;
        }
        indev = lv_indev_get_next(indev);
    }
    return NULL;
}
#endif

// App-level keyboard toggling now only attaches/detaches the LVGL input
// device — the I2C keyboard hardware itself is initialized once at boot
// (via instance.begin()) and re-initialized only on sleep/wake through
// hw_power_up_all(). Re-running initKeyboard() on every app transition
// caused a brief backlight flash on home entry (the vendor init applies a
// non-zero default before our user-configured brightness is restored), and
// also risked re-attaching the TCA8418 ISR which can hang the chip.
static bool s_keyboard_enabled = false;

void disable_keyboard()
{
    if (!s_keyboard_enabled) return;
    if (hw_has_keyboard()) {
        lv_indev_enable(lv_get_keyboard_indev(), false);
    }
    s_keyboard_enabled = false;
}

void enable_keyboard()
{
    if (s_keyboard_enabled) return;
    if (hw_has_keyboard()) {
        lv_indev_enable(lv_get_keyboard_indev(), true);
    }
    s_keyboard_enabled = true;
}

LV_FONT_DECLARE(lv_font_montserrat_10);
LV_FONT_DECLARE(lv_font_montserrat_12);
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_22);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_30);
LV_FONT_DECLARE(lv_font_montserrat_32);
LV_FONT_DECLARE(lv_font_unscii_8);
LV_FONT_DECLARE(lv_font_unscii_16);
LV_FONT_DECLARE(ui_font_courier_10);
LV_FONT_DECLARE(ui_font_courier_12);
LV_FONT_DECLARE(ui_font_courier_14);
LV_FONT_DECLARE(ui_font_courier_16);
LV_FONT_DECLARE(ui_font_courier_20);
LV_FONT_DECLARE(ui_font_courier_24);
LV_FONT_DECLARE(font_inter_14);
LV_FONT_DECLARE(font_inter_16);
LV_FONT_DECLARE(font_inter_18);
LV_FONT_DECLARE(font_inter_20);
LV_FONT_DECLARE(font_emoji_16);
LV_FONT_DECLARE(font_emoji_20);
LV_FONT_DECLARE(font_atkinson_14);
LV_FONT_DECLARE(font_atkinson_16);
LV_FONT_DECLARE(font_atkinson_18);
LV_FONT_DECLARE(font_atkinson_20);
LV_FONT_DECLARE(font_jbmono_10);
LV_FONT_DECLARE(font_jbmono_12);
LV_FONT_DECLARE(font_jbmono_14);
LV_FONT_DECLARE(font_jbmono_16);
LV_FONT_DECLARE(font_jbmono_20);
LV_FONT_DECLARE(font_jbmono_24);

// Installed fonts (Courier, Atkinson, JetBrains, Inter) were converted with
// only ASCII / Latin-1 ranges, so LVGL symbol glyphs — LV_SYMBOL_* live in
// the 0xF000-0xF8FF private-use area — render as empty boxes when those
// faces are selected. Montserrat is generated with FontAwesome alongside
// ASCII, so we copy each installed font into a mutable slot and point
// .fallback at the matching Montserrat size. LVGL walks the fallback chain
// for any codepoint the base face lacks, which lets icons render at the
// right size regardless of the user's font choice.
static lv_font_t s_courier_10_ic, s_courier_12_ic, s_courier_14_ic;
static lv_font_t s_courier_16_ic, s_courier_20_ic, s_courier_24_ic;
static lv_font_t s_atkinson_14_ic, s_atkinson_16_ic, s_atkinson_18_ic, s_atkinson_20_ic;
static lv_font_t s_jbmono_10_ic, s_jbmono_12_ic, s_jbmono_14_ic;
static lv_font_t s_jbmono_16_ic, s_jbmono_20_ic, s_jbmono_24_ic;
static lv_font_t s_inter_14_ic, s_inter_16_ic, s_inter_18_ic, s_inter_20_ic;
static bool s_icon_fallback_ready = false;

static void init_icon_fallback_fonts()
{
    if (s_icon_fallback_ready) return;
    s_courier_10_ic  = ui_font_courier_10; s_courier_10_ic.fallback  = &lv_font_montserrat_10;
    s_courier_12_ic  = ui_font_courier_12; s_courier_12_ic.fallback  = &lv_font_montserrat_12;
    s_courier_14_ic  = ui_font_courier_14; s_courier_14_ic.fallback  = &lv_font_montserrat_14;
    s_courier_16_ic  = ui_font_courier_16; s_courier_16_ic.fallback  = &lv_font_montserrat_16;
    s_courier_20_ic  = ui_font_courier_20; s_courier_20_ic.fallback  = &lv_font_montserrat_20;
    s_courier_24_ic  = ui_font_courier_24; s_courier_24_ic.fallback  = &lv_font_montserrat_24;
    s_atkinson_14_ic = font_atkinson_14;   s_atkinson_14_ic.fallback = &lv_font_montserrat_14;
    s_atkinson_16_ic = font_atkinson_16;   s_atkinson_16_ic.fallback = &lv_font_montserrat_16;
    s_atkinson_18_ic = font_atkinson_18;   s_atkinson_18_ic.fallback = &lv_font_montserrat_18;
    s_atkinson_20_ic = font_atkinson_20;   s_atkinson_20_ic.fallback = &lv_font_montserrat_20;
    s_jbmono_10_ic   = font_jbmono_10;     s_jbmono_10_ic.fallback   = &lv_font_montserrat_10;
    s_jbmono_12_ic   = font_jbmono_12;     s_jbmono_12_ic.fallback   = &lv_font_montserrat_12;
    s_jbmono_14_ic   = font_jbmono_14;     s_jbmono_14_ic.fallback   = &lv_font_montserrat_14;
    s_jbmono_16_ic   = font_jbmono_16;     s_jbmono_16_ic.fallback   = &lv_font_montserrat_16;
    s_jbmono_20_ic   = font_jbmono_20;     s_jbmono_20_ic.fallback   = &lv_font_montserrat_20;
    s_jbmono_24_ic   = font_jbmono_24;     s_jbmono_24_ic.fallback   = &lv_font_montserrat_24;
    s_inter_14_ic    = font_inter_14;      s_inter_14_ic.fallback    = &lv_font_montserrat_14;
    s_inter_16_ic    = font_inter_16;      s_inter_16_ic.fallback    = &lv_font_montserrat_16;
    s_inter_18_ic    = font_inter_18;      s_inter_18_ic.fallback    = &lv_font_montserrat_18;
    s_inter_20_ic    = font_inter_20;      s_inter_20_ic.fallback    = &lv_font_montserrat_20;
    s_icon_fallback_ready = true;
}

// Inter-with-emoji-fallback variants used only by the Telegram app. The
// chain is Inter -> emoji -> Montserrat so chat messages render glyphs,
// color emoji, AND LVGL icons. 14/16 fall back to emoji_16; 18/20 fall
// back to emoji_20 to keep line heights roughly aligned.
static lv_font_t s_inter_emoji_14;
static lv_font_t s_inter_emoji_16;
static lv_font_t s_inter_emoji_18;
static lv_font_t s_inter_emoji_20;
static lv_font_t s_emoji_16_ic;
static lv_font_t s_emoji_20_ic;
static bool s_inter_emoji_ready = false;

static void init_inter_emoji_fonts()
{
    if (s_inter_emoji_ready) return;
    s_emoji_16_ic = font_emoji_16; s_emoji_16_ic.fallback = &lv_font_montserrat_16;
    s_emoji_20_ic = font_emoji_20; s_emoji_20_ic.fallback = &lv_font_montserrat_20;
    s_inter_emoji_14 = font_inter_14; s_inter_emoji_14.fallback = &s_emoji_16_ic;
    s_inter_emoji_16 = font_inter_16; s_inter_emoji_16.fallback = &s_emoji_16_ic;
    s_inter_emoji_18 = font_inter_18; s_inter_emoji_18.fallback = &s_emoji_20_ic;
    s_inter_emoji_20 = font_inter_20; s_inter_emoji_20.fallback = &s_emoji_20_ic;
    s_inter_emoji_ready = true;
}

// idx: 0=Montserrat, 1=Unscii 8, 2=Unscii 16, 3=Courier, 4=Inter,
//      5=Atkinson Hyperlegible, 6=JetBrains Mono
static const lv_font_t *pick_font(uint8_t idx, uint8_t size)
{
    if (idx == 1) return &lv_font_unscii_8;
    if (idx == 2) return &lv_font_unscii_16;
    if (idx >= 3 && idx <= 6) init_icon_fallback_fonts();
    if (idx == 3) {
        if (size <= 10) return &s_courier_10_ic;
        if (size <= 12) return &s_courier_12_ic;
        if (size <= 14) return &s_courier_14_ic;
        if (size <= 16) return &s_courier_16_ic;
        if (size <= 20) return &s_courier_20_ic;
        return &s_courier_24_ic;
    }
    if (idx == 4) {
        if (size <= 14) return &s_inter_14_ic;
        if (size <= 16) return &s_inter_16_ic;
        if (size <= 18) return &s_inter_18_ic;
        return &s_inter_20_ic;
    }
    if (idx == 5) {
        if (size <= 14) return &s_atkinson_14_ic;
        if (size <= 16) return &s_atkinson_16_ic;
        if (size <= 18) return &s_atkinson_18_ic;
        return &s_atkinson_20_ic;
    }
    if (idx == 6) {
        if (size <= 10) return &s_jbmono_10_ic;
        if (size <= 12) return &s_jbmono_12_ic;
        if (size <= 14) return &s_jbmono_14_ic;
        if (size <= 16) return &s_jbmono_16_ic;
        if (size <= 20) return &s_jbmono_20_ic;
        return &s_jbmono_24_ic;
    }
    // Default to Montserrat
    switch (size) {
    case 10: return &lv_font_montserrat_10;
    case 12: return &lv_font_montserrat_12;
    case 14: return &lv_font_montserrat_14;
    case 16: return &lv_font_montserrat_16;
    case 18: return &lv_font_montserrat_18;
    case 20: return &lv_font_montserrat_20;
    case 22: return &lv_font_montserrat_22;
    case 24: return &lv_font_montserrat_24;
    case 26: return &lv_font_montserrat_26;
    case 28: return &lv_font_montserrat_28;
    case 30: return &lv_font_montserrat_30;
    case 32: return &lv_font_montserrat_32;
    default: return &lv_font_montserrat_14;
    }
}

const lv_font_t *get_editor_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.editor_font_index, settings.editor_font_size);
}

const lv_font_t *get_small_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);

    uint8_t idx = settings.editor_font_index;
    uint8_t size = settings.editor_font_size;

    if (idx == 2) return &lv_font_unscii_8; // Unscii 16 -> 8

    // For scalable faces, go 2 sizes smaller, min 10
    if (idx == 0 || idx == 4 || idx == 5 || idx == 6) {
        int s = (int)size - 2;
        if (s < 10) s = 10;
        size = (uint8_t)s;
    }
    return pick_font(idx, size);
}

const lv_font_t *get_journal_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.journal_font_index, settings.journal_font_size);
}

const lv_font_t *get_header_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.header_font_index, settings.header_font_size);
}

const lv_font_t *get_home_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.home_font_index, settings.home_font_size);
}

const lv_font_t *get_system_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.system_font_index, settings.system_font_size);
}

const lv_font_t *get_weather_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.weather_font_index, settings.weather_font_size);
}

const lv_font_t *get_telegram_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    if (settings.telegram_font_index == 4) {
        init_inter_emoji_fonts();
        uint8_t size = settings.telegram_font_size;
        if (size <= 14) return &s_inter_emoji_14;
        if (size <= 16) return &s_inter_emoji_16;
        if (size <= 18) return &s_inter_emoji_18;
        return &s_inter_emoji_20;
    }
    return pick_font(settings.telegram_font_index, settings.telegram_font_size);
}

const lv_font_t *get_ssh_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.ssh_font_index, settings.ssh_font_size);
}

const lv_font_t *get_chat_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    return pick_font(settings.chat_font_index, settings.chat_font_size);
}

const lv_font_t *get_telegram_list_font()
{
    user_setting_params_t settings;
    hw_get_user_setting(settings);
    // Chat list items render with a noticeably larger font than the body
    // text — the list is the main nav, names need to be thumb-legible.
    uint8_t size = settings.telegram_font_size + 6;
    if (settings.telegram_font_index == 4) {
        init_inter_emoji_fonts();
        if (size <= 14) return &s_inter_emoji_14;
        if (size <= 16) return &s_inter_emoji_16;
        if (size <= 18) return &s_inter_emoji_18;
        return &s_inter_emoji_20;
    }
    return pick_font(settings.telegram_font_index, size);
}
