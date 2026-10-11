/**
 * @file      settings_display.cpp
 * @brief     Settings » Display & Backlight / Charger / Performance subpages,
 *            plus the UI style (Classic / Pixel Iso / Hotel / BeOS) selector
 *            and the dark-mode toggle.
 *            Extracted from ui_settings.cpp; see settings_internal.h for the
 *            cross-TU contract.
 *
 * Bundled because each subpage is small on its own (<80 lines) and they
 * all share the same dependency surface: `local_param`, `invert_scroll_key_cb`,
 * `register_subpage_group_obj`, and `create_toggle_btn_row`. No cached LVGL
 * state, so no reset_state needed.
 */
#include "../ui_define.h"
#include "settings_internal.h"
#include "../core/system.h"

namespace display_cfg {

namespace {

long map_r(long x, long in_min, long in_max, long out_min, long out_max)
{
    if (x < in_min) {
        return out_min;
    } else if (x > in_max) {
        return out_max;
    }
    return ((x - in_min) * (out_max - out_min)) / (in_max - in_min) + out_min;
}

// --- Backlight callbacks ---

void display_brightness_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val =  lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);

    uint16_t min_brightness = hw_get_disp_min_brightness();
    uint16_t max_brightness = hw_get_disp_max_brightness();

    lv_label_set_text_fmt(slider_label, "   %u%%  ", map_r(val, min_brightness, max_brightness, 0, 100));
    local_param.brightness_level = val;
    hw_set_disp_backlight(val);
}

void keyboard_brightness_cb(lv_event_t *e)
{
    // Slider is 0..20 so each encoder tick moves 5% of full range.
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    int32_t step = lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);
    uint8_t val = (uint8_t)map_r(step, 0, 20, 0, 255);
    lv_label_set_text_fmt(slider_label, "   %ld%%  ", step * 5);
    local_param.keyboard_bl_level = val;
    hw_set_kb_backlight(val);
}

void disp_timeout_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val =  lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);

    local_param.disp_timeout_second = val;
    if (val == 0) {
        lv_label_set_text(slider_label, " Always ");
    } else {
        lv_label_set_text_fmt(slider_label, "   %uS  ", val);
    }
}

void sleep_mode_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint8_t val = (uint8_t)lv_slider_get_value(obj);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(obj);

    local_param.sleep_mode = val;
    if (val == 0) {
        lv_label_set_text(slider_label, " Light ");
    } else {
        lv_label_set_text(slider_label, " Deep ");
    }
}

void show_mem_usage_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        local_param.show_mem_usage = turnOn;
        hw_set_user_setting(local_param);
    }
}

void show_file_count_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        local_param.show_file_count = turnOn;
        hw_set_user_setting(local_param);
    }
}

// --- Charger / OTG callbacks ---

void otg_output_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        printf("State: %s\n", turnOn ? "On" : "Off");
        // Rejected: un-check, and the row's label follows (" Off ").
        if (hw_set_otg(turnOn) == false) {
            lv_obj_remove_state(obj, LV_STATE_CHECKED);
        }
    }
}

void charger_enable_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        local_param.charger_enable = turnOn;
        printf("State: %s\n", turnOn ? "On" : "Off");

        // Persist eagerly so the background battery loop sees the new state
        // on its next tick, then refresh battery-history to re-apply limits.
        hw_set_user_setting(local_param);
        hw_set_charger(turnOn);
        hw_update_battery_history();
    }
}

void charger_current_cb(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *slider_label = (lv_obj_t *)lv_obj_get_user_data(slider);
    int32_t val = lv_slider_get_value(slider);
    local_param.charger_current = hw_set_charger_current_level(val);
    lv_label_set_text_fmt(slider_label, "%04umA", local_param.charger_current);
    // The current is applied to the PMIC live above. Like the sibling display
    // sliders, do NOT persist per notch — dragging fires this callback on every
    // step, and hw_set_user_setting() rewrites the whole NVS blob each time.
    // local_param is flushed once when the user leaves Settings (ui_sys_exit),
    // so the setting still survives without thrashing flash on every drag.
}

void charge_limit_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    bool turnOn = lv_obj_has_state(btn, LV_STATE_CHECKED);
    local_param.charge_limit_en = turnOn;

    hw_set_user_setting(local_param);

    if (!turnOn) {
        // Restoring the raw user preference — charger_enable stays authoritative
        // once the 80% cap is off.
        hw_set_charger(local_param.charger_enable);
    } else {
        hw_update_battery_history();
    }
}

// --- Performance callbacks ---

void cpu_freq_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint16_t index = lv_dropdown_get_selected(obj);
    uint16_t freqs[] = {240, 160, 80};
    local_param.cpu_freq_mhz = freqs[index];
    hw_set_user_setting(local_param);
}

// --- UI style callback ---

void ui_style_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    ui_style_set((ui_style_t)lv_dropdown_get_selected(obj));
    core::System::getInstance().applyChromeStyle();
}

void dark_mode_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
        ui_dark_mode_set(turnOn);
        core::System::getInstance().applyChromeStyle();
    }
}

} // anonymous namespace

void build_backlight(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    lv_obj_t *slider;
    lv_obj_t *parent;
    lv_obj_t *slider_label;

    uint16_t min_brightness = hw_get_disp_min_brightness();
    uint16_t max_brightness = hw_get_disp_max_brightness();

    auto add_slider = [&](const char *txt, int32_t min, int32_t max, int32_t val,
                          lv_event_cb_t cb, const char *fmt_str) {
        slider = create_slider(sub_page, txt, min, max, val, cb);
        parent = lv_obj_get_parent(slider);
        slider_label = lv_label_create(parent);
        char buf[16];
        snprintf(buf, sizeof(buf), fmt_str, (int)map_r(val, min, max, 0, 100));
        lv_label_set_text(slider_label, buf);
        lv_obj_set_user_data(slider, slider_label);
        register_subpage_group_obj(sub_page, slider);
    };

    add_slider("Screen", min_brightness, max_brightness,
               local_param.brightness_level, display_brightness_cb, "%d%%");
    lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);

    if (hw_has_keyboard()) {
        // 0..20 slider range = 21 discrete 5% steps; each encoder tick moves 5%.
        int32_t kb_step = (int32_t)map_r(local_param.keyboard_bl_level, 0, 255, 0, 20);
        add_slider("Keyboard", 0, 20, kb_step, keyboard_brightness_cb, "%d%%");
        lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                            (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
    }

    slider = create_slider(sub_page, "Timeout", 0, 180,
                           local_param.disp_timeout_second, disp_timeout_cb);
    parent = lv_obj_get_parent(slider);
    slider_label = lv_label_create(parent);
    if (local_param.disp_timeout_second == 0) {
        lv_label_set_text(slider_label, " Always ");
    } else {
        lv_label_set_text_fmt(slider_label, "   %uS  ", local_param.disp_timeout_second);
    }
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);
    lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);

    slider = create_slider(sub_page, "Sleep Mode", 0, 1,
                           local_param.sleep_mode, sleep_mode_cb);
    parent = lv_obj_get_parent(slider);
    slider_label = lv_label_create(parent);
    if (local_param.sleep_mode == 0) {
        lv_label_set_text(slider_label, " Light ");
    } else {
        lv_label_set_text(slider_label, " Deep ");
    }
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);

    create_toggle_btn_row(sub_page, "Show Memory", local_param.show_mem_usage, show_mem_usage_cb);
    create_toggle_btn_row(sub_page, "Show File Count", local_param.show_file_count, show_file_count_cb);

    // UI style: applies to newly built screens immediately and fully on
    // leaving Settings (ui_sys_exit rebuilds the theme + chrome).
    lv_obj_t *dd = create_dropdown(sub_page, "UI Style",
                                   "Classic\nPixel Iso\nHotel\nBeOS",
                                   (uint16_t)ui_style_get(), ui_style_cb);
    lv_obj_t *row = lv_obj_get_parent(dd);
    lv_obj_t *title_label = lv_obj_get_child(row, 0);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_set_width(title_label, 0);
    lv_obj_set_flex_grow(dd, 0);
    lv_obj_set_width(dd, LV_PCT(45));
    register_subpage_group_obj(sub_page, dd);

    // Dark mode: night palette for Hotel / BeOS; Classic and Pixel Iso are
    // dark already. Same apply-on-exit behaviour as the style above.
    create_toggle_btn_row(sub_page, "Dark Mode", ui_dark_mode_get(), dark_mode_cb);
}

void build_otg(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    bool enableOtg = hw_get_otg_enable();
    uint8_t total_charge_level = hw_get_charge_level_nums();
    uint8_t curr_charge_level = hw_get_charger_current_level();

    if (hw_has_otg_function()) {
        create_toggle_btn_row(sub_page, "OTG Output", enableOtg, otg_output_cb);
    }

    create_toggle_btn_row(sub_page, "Charging", local_param.charger_enable, charger_enable_cb);

    lv_obj_t *slider = create_slider(sub_page, "Current",
                                     1, total_charge_level, curr_charge_level,
                                     charger_current_cb);
    lv_obj_t *parent = lv_obj_get_parent(slider);
    lv_obj_t *slider_label = lv_label_create(parent);
    lv_label_set_text_fmt(slider_label, "%umA", local_param.charger_current);
    lv_obj_set_user_data(slider, slider_label);
    register_subpage_group_obj(sub_page, slider);
    lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);

    create_toggle_btn_row(sub_page, "Limit 80%", local_param.charge_limit_en, charge_limit_cb);
}

void build_performance(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    const char *freq_options = "240 MHz (Max)\n160 MHz\n80 MHz (Power)";
    uint16_t current_freq = local_param.cpu_freq_mhz;
    uint8_t sel_idx = 0; // Default 240
    if (current_freq == 160) sel_idx = 1;
    else if (current_freq == 80) sel_idx = 2;

    lv_obj_t *dd = create_dropdown(sub_page, "CPU Clock", freq_options, sel_idx, cpu_freq_cb);

    // Size the dropdown wide enough to comfortably show the longest
    // option ("240 MHz (Max)") plus the arrow, but not stretched across
    // the whole row. Title label absorbs the remaining width.
    lv_obj_t *row = lv_obj_get_parent(dd);
    lv_obj_t *title_label = lv_obj_get_child(row, 0);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_set_width(title_label, 0);
    lv_obj_set_flex_grow(dd, 0);
    lv_obj_set_width(dd, LV_PCT(45));

    register_subpage_group_obj(sub_page, dd);
}

} // namespace display_cfg
