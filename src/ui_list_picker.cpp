/**
 * @file      ui_list_picker.cpp
 * @brief     Modal list-picker overlay.
 *
 * Built on ui_overlay_open (same look/feel as ui_wifi.cpp): sits on
 * lv_layer_top below the status bar, takes over the back button to close, and focuses the first
 * item so keyboard/encoder navigation Just Works. Only one picker can be
 * open at a time — subsequent opens are ignored while one is live.
 */
#include "ui_list_picker.h"
#include "ui_define.h"

#include <memory>

namespace {

struct PickerCtx {
    ui_overlay_t ov        = {};
    lv_obj_t   *list       = nullptr;
    ui_list_picker_cb cb = nullptr;
    void *ud = nullptr;
    bool fired = false;
};

static std::unique_ptr<PickerCtx> g_ctx;

static void tear_down(int picked_index)
{
    if (!g_ctx) return;
    /* Move ownership into a local so g_ctx is null while the user callback
     * runs — letting it safely reopen the picker without racing our delete. */
    std::unique_ptr<PickerCtx> ctx = std::move(g_ctx);

    ui_list_picker_cb cb = ctx->cb;
    void *ud = ctx->ud;
    bool will_fire = !ctx->fired && cb;
    ctx->fired = true;

    ui_overlay_close(&ctx->ov);
    ctx.reset();

    if (will_fire) cb(picked_index, ud);
}

static void close_cb(lv_event_t *) { tear_down(-1); }

static void item_clicked_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (!btn || !g_ctx) return;
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);
    tear_down((int)idx);
}

} // namespace

void ui_list_picker_open(const char *title,
                         const std::vector<std::string> &items,
                         ui_list_picker_cb cb, void *ud)
{
    if (g_ctx) {
        // A picker is already up. Don't stack; fire cancel for the caller.
        if (cb) cb(-1, ud);
        return;
    }

    g_ctx = std::make_unique<PickerCtx>();
    PickerCtx *ctx = g_ctx.get();
    ctx->cb = cb;
    ctx->ud = ud;

    lv_obj_t *overlay = ui_overlay_open(&ctx->ov, close_cb, false);

    if (title && *title) {
        lv_obj_t *t = lv_label_create(overlay);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_color(t, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(t, lv_pct(100));
    }

    ctx->list = lv_list_create(overlay);
    lv_obj_set_width(ctx->list, lv_pct(100));
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_set_style_radius(ctx->list, UI_RADIUS, 0);

    if (items.empty()) {
        lv_obj_t *empty = lv_label_create(ctx->list);
        lv_label_set_text(empty, "(no results)");
        lv_obj_set_style_text_color(empty, UI_COLOR_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
        return;
    }

    lv_obj_t *first = nullptr;
    for (size_t i = 0; i < items.size(); ++i) {
        lv_obj_t *btn = lv_list_add_btn(ctx->list, LV_SYMBOL_RIGHT, items[i].c_str());
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, item_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_group_add_obj(ctx->ov.group, btn);
        if (!first) first = btn;
    }
    if (first) lv_group_focus_obj(first);
}
