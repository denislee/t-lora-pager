/**
 * @file      settings_hub.cpp
 * @brief     Settings » Local Hub subpage. Centralized lilyhub config —
 *            replaces the inline "Local hub URL" entry that used to live
 *            under Settings » Weather. See settings_internal.h for the
 *            cross-TU contract.
 *
 * Storage is owned by hal/hub.{h,cpp}. This file is purely UI: a master
 * enable toggle, a URL input, and a clear button.
 */
#include "../ui_define.h"
#include "../hal/hub.h"
#include "../hal/str_encode.h"
#include "settings_internal.h"

namespace hub_cfg {

static lv_obj_t *g_url_label   = nullptr;
static lv_obj_t *g_status_label = nullptr;

void reset_state()
{
    g_url_label = nullptr;
    g_status_label = nullptr;
}

static void refresh_url_label()
{
    if (!g_url_label) return;
    std::string url = hal::hub_get_url_raw();
    lv_label_set_text_fmt(g_url_label, "URL: %s",
                          url.empty() ? "(not set)" : url.c_str());
}

static void set_status(const char *text, lv_color_t color)
{
    if (!g_status_label) return;
    lv_label_set_text(g_status_label, text ? text : "");
    lv_obj_set_style_text_color(g_status_label, color, 0);
}

static void enable_toggle_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool en = lv_obj_has_state(obj, LV_STATE_CHECKED);
    settings_commit_bool("hub.enabled", en);
    if (en && hal::hub_get_url_raw().empty()) {
        set_status("Set a URL to start using the hub",
                   lv_palette_main(LV_PALETTE_ORANGE));
    } else {
        set_status(en ? "Hub on - used first, falls back direct"
                      : "Hub off - direct internet only",
                   en ? lv_palette_main(LV_PALETTE_GREEN) : UI_COLOR_MUTED);
    }
}

static void url_entered_cb(const char *text, void *ud)
{
    (void)ud;
    if (!text) { set_status("Cancelled", UI_COLOR_MUTED); return; }
    std::string v = hal::trim(text);
    std::string why;
    if (!settings_commit("hub.url", settings::Value::of_str(v), false, &why)) {
        refresh_url_label();
        set_status(why.c_str(), lv_palette_main(LV_PALETTE_RED));
        return;
    }
    refresh_url_label();
    if (v.empty()) {
        set_status("URL cleared", UI_COLOR_MUTED);
    } else if (!hal::hub_get_enabled_pref()) {
        set_status("URL saved - toggle on to use",
                   lv_palette_main(LV_PALETTE_ORANGE));
    } else {
        set_status("URL saved - hub active",
                   lv_palette_main(LV_PALETTE_GREEN));
    }
}

static void btn_set_url_cb(lv_event_t *e)
{
    (void)e;
    std::string current = hal::hub_get_url_raw();
    ui_text_prompt("Local hub URL",
                   "e.g. http://192.168.1.10:8080 (empty = clear)",
                   current.c_str(), url_entered_cb, nullptr);
}

static void btn_clear_url_cb(lv_event_t *e)
{
    (void)e;
    settings_commit_str("hub.url", "");
    refresh_url_label();
    set_status("URL cleared", UI_COLOR_MUTED);
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    create_toggle_btn_row(sub_page, "Use local hub", hal::hub_get_enabled_pref(),
                          enable_toggle_cb);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    g_url_label = lv_label_create(status);
    lv_obj_set_style_text_color(g_url_label, UI_COLOR_MUTED, 0);
    lv_label_set_long_mode(g_url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_url_label, LV_PCT(100));
    refresh_url_label();

    lv_obj_t *b1 = create_button(sub_page, LV_SYMBOL_KEYBOARD,
                                 "Set hub URL", btn_set_url_cb);
    register_subpage_group_obj(sub_page, b1);

    lv_obj_t *b2 = create_button(sub_page, LV_SYMBOL_CLOSE,
                                 "Clear URL", btn_clear_url_cb);
    register_subpage_group_obj(sub_page, b2);

    g_status_label = lv_label_create(sub_page);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_status_label, LV_PCT(100));
    lv_label_set_text(g_status_label, "");
    lv_obj_set_style_text_color(g_status_label, UI_COLOR_MUTED, 0);
}

} // namespace hub_cfg
