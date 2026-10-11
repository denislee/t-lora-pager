/**
 * @file      settings_fonts.cpp
 * @brief     Settings » Fonts subpage. Extracted from ui_settings.cpp; see
 *            settings_internal.h for the cross-TU contract.
 *
 * 18 dropdowns (9 UI contexts × face/size) all mutating fields on
 * `local_param` and committing via `hw_set_user_setting` so a crash/power-
 * loss between clicks can't strand the user with a half-applied font
 * change.
 */
#include "../ui_define.h"
#include "settings_internal.h"

namespace fonts_cfg {

// Only "Inter" carries the Latin-1 supplement (U+00A0..U+00FF), so it's the
// face to pick when rendering Portuguese, Spanish, French, or other accented
// Latin scripts. The label below advertises that so users know which to pick.
static const char *FONT_FACE_OPTIONS = "Montserrat\nUnscii 8\nUnscii 16\nCourier\nInter (Latin-1)\nAtkinson (Latin-1)\nJetBrains Mono";
static const char *FONT_SIZE_OPTIONS = "10\n12\n14\n16\n18\n20\n22\n24\n26\n28\n30\n32";

static uint8_t size_to_idx(uint8_t size)
{
    if (size < 10 || size > 32) return 2; // Default 14
    return (size - 10) / 2;
}

// Persist every font change right away so the selection survives unexpected
// exits (crash, power loss, app switch that bypasses ui_sys_exit). The full
// blob is small and NVS writes are cheap enough for interactive use.
static void commit_font_change()
{
    hw_set_user_setting(local_param);
}

// Both callbacks get a pointer to the `local_param` field they own as
// user_data. Both end with lv_event_stop_processing, as every per-field
// callback did before (the Telegram pair was the one exception).
static void font_face_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t *field = (uint8_t *)lv_event_get_user_data(e);
    *field = lv_dropdown_get_selected(obj);
    commit_font_change();
    lv_event_stop_processing(e);
}

static void font_size_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t *field = (uint8_t *)lv_event_get_user_data(e);
    *field = 10 + lv_dropdown_get_selected(obj) * 2;
    commit_font_change();
    lv_event_stop_processing(e);
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    auto add_font_section = [&](const char *title, uint8_t &face_idx, uint8_t &size) {
        lv_obj_t *row = lv_obj_create(sub_page);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, title);
        lv_obj_set_style_text_color(name, UI_COLOR_MUTED, 0);
        lv_obj_set_width(name, LV_PCT(26));

        lv_obj_t *dd_face = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_face, FONT_FACE_OPTIONS);
        lv_dropdown_set_selected(dd_face, face_idx);
        lv_obj_set_width(dd_face, 0);
        lv_obj_set_flex_grow(dd_face, 2);
        lv_obj_add_event_cb(dd_face, font_face_cb, LV_EVENT_VALUE_CHANGED, &face_idx);
        register_subpage_group_obj(sub_page, dd_face);

        lv_obj_t *dd_size = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_size, FONT_SIZE_OPTIONS);
        lv_dropdown_set_selected(dd_size, size_to_idx(size));
        lv_obj_set_width(dd_size, 0);
        lv_obj_set_flex_grow(dd_size, 1);
        lv_obj_add_event_cb(dd_size, font_size_cb, LV_EVENT_VALUE_CHANGED, &size);
        register_subpage_group_obj(sub_page, dd_size);
    };

    add_font_section("System",   local_param.system_font_index,   local_param.system_font_size);
    add_font_section("Home",     local_param.home_font_index,     local_param.home_font_size);
    add_font_section("Notes",    local_param.editor_font_index,   local_param.editor_font_size);
    add_font_section("Journal",  local_param.journal_font_index,  local_param.journal_font_size);
    add_font_section("Weather",  local_param.weather_font_index,  local_param.weather_font_size);
    add_font_section("Telegram", local_param.telegram_font_index, local_param.telegram_font_size);
    add_font_section("SSH",      local_param.ssh_font_index,      local_param.ssh_font_size);
    add_font_section("Chat",     local_param.chat_font_index,     local_param.chat_font_size);
    add_font_section("Header",   local_param.header_font_index,   local_param.header_font_size);
}

} // namespace fonts_cfg
