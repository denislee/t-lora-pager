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

// CPU clock options, in dropdown order.
const uint16_t kCpuFreqs[] = {240, 160, 80};

// --- Slider value labels: one formatter per row, used for the initial text
// and by the slider's callback, so the label never changes shape on the
// first notch. ---

void set_pct_label(lv_obj_t *label, long pct)
{
    lv_label_set_text_fmt(label, "   %ld%%  ", pct);
}

void set_brightness_label(lv_obj_t *label, int32_t val)
{
    set_pct_label(label, map_r(val, hw_get_disp_min_brightness(),
                               hw_get_disp_max_brightness(), 0, 100));
}

void set_timeout_label(lv_obj_t *label, unsigned seconds)
{
    if (seconds == 0) lv_label_set_text(label, " Always ");
    else              lv_label_set_text_fmt(label, "   %uS  ", seconds);
}

void set_sleep_mode_label(lv_obj_t *label, unsigned mode)
{
    lv_label_set_text(label, mode == 0 ? " Light " : " Deep ");
}

void set_charger_current_label(lv_obj_t *label, unsigned ma)
{
    lv_label_set_text_fmt(label, "%04umA", ma);
}

// Slider row with a value label to its right (the slider's user_data, which
// the callbacks above read back). Registered with `sub_page`'s focus group;
// `invert_scroll` adds the encoder-direction fix the value sliders use.
// Returns the value label.
lv_obj_t *add_slider(lv_obj_t *sub_page, const char *txt, int32_t min, int32_t max,
                     int32_t val, lv_event_cb_t cb, bool invert_scroll = true)
{
    lv_obj_t *slider = create_slider(sub_page, txt, min, max, val, cb);
    lv_obj_t *label = lv_label_create(lv_obj_get_parent(slider));
    lv_obj_set_user_data(slider, label);
    register_subpage_group_obj(sub_page, slider);
    if (invert_scroll) {
        lv_obj_add_event_cb(slider, invert_scroll_key_cb,
                            (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), NULL);
    }
    return label;
}

// Dropdown sized wide enough for its longest option plus the arrow, but not
// stretched across the whole row; the title label absorbs the rest.
void size_dropdown_row(lv_obj_t *dd)
{
    if (ui_is_narrow()) return;  // create_dropdown already stacked it
    lv_obj_t *row = lv_obj_get_parent(dd);
    lv_obj_t *title_label = lv_obj_get_child(row, 0);
    lv_obj_set_flex_grow(title_label, 1);
    lv_obj_set_width(title_label, 0);
    lv_obj_set_flex_grow(dd, 0);
    lv_obj_set_width(dd, LV_PCT(45));
}

lv_obj_t *slider_label_of(lv_event_t *e, int32_t *val)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    *val = lv_slider_get_value(obj);
    return (lv_obj_t *)lv_obj_get_user_data(obj);
}

// --- Backlight callbacks ---

void display_brightness_cb(lv_event_t *e)
{
    int32_t v;
    lv_obj_t *slider_label = slider_label_of(e, &v);
    uint8_t val = (uint8_t)v;
    set_brightness_label(slider_label, val);
    settings_commit_later("display.brightness", settings::Value::of_int(val));
    hw_set_disp_backlight(val);
}

void keyboard_brightness_cb(lv_event_t *e)
{
    // Slider is 0..20 so each encoder tick moves 5% of full range.
    int32_t step;
    lv_obj_t *slider_label = slider_label_of(e, &step);
    uint8_t val = (uint8_t)map_r(step, 0, 20, 0, 255);
    set_pct_label(slider_label, (long)step * 5);
    settings_commit_later("display.kb_backlight", settings::Value::of_int((long)step * 5));
    hw_set_kb_backlight(val);
}

void disp_timeout_cb(lv_event_t *e)
{
    int32_t v;
    lv_obj_t *slider_label = slider_label_of(e, &v);
    uint8_t val = (uint8_t)v;
    settings_commit_later("display.timeout_s", settings::Value::of_int(val));
    set_timeout_label(slider_label, val);
}

void sleep_mode_cb(lv_event_t *e)
{
    int32_t v;
    lv_obj_t *slider_label = slider_label_of(e, &v);
    uint8_t val = (uint8_t)v;
    settings_commit_later("display.sleep_mode", settings::Value::of_str(val ? "deep" : "light"));
    set_sleep_mode_label(slider_label, val);
}

// Show Memory / Show File Count: event user_data is the settings registry id
// (a const char *). create_toggle_btn_row registers only
// LV_EVENT_VALUE_CHANGED.
void u8_toggle_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    const char *id = (const char *)lv_event_get_user_data(e);
    settings_commit_bool(id, lv_obj_has_state(obj, LV_STATE_CHECKED));
}

// --- Charger / OTG callbacks ---

void otg_output_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);
    // Rejected: un-check, and the row's label follows (" Off ").
    // The registry setter calls hw_set_otg itself.
    if (!settings_commit("power.otg", settings::Value::of_bool(turnOn))) {
        lv_obj_remove_state(obj, LV_STATE_CHECKED);
    }
}

void charger_enable_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool turnOn = lv_obj_has_state(obj, LV_STATE_CHECKED);

    // Store eagerly through the registry so the background charge-limit check
    // sees the new state on its next tick, then re-apply the limit now.
    settings_commit_bool("power.charging", turnOn);
    hw_set_charger(turnOn);
    hw_apply_charge_limit();
}

void charger_current_cb(lv_event_t *e)
{
    int32_t val;
    lv_obj_t *slider_label = slider_label_of(e, &val);
    // Applied live per notch; stored once the slider settles
    // (settings_commit_later).
    uint16_t ma = hw_set_charger_current_level(val);
    set_charger_current_label(slider_label, ma);
    settings_commit_later("power.charge_current_ma", settings::Value::of_int(ma));
}

void charge_limit_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    bool turnOn = lv_obj_has_state(btn, LV_STATE_CHECKED);
    settings_commit_bool("power.charge_limit_80", turnOn);

    if (!turnOn) {
        // Restoring the raw user preference — charger_enable stays authoritative
        // once the 80% cap is off. local_param was refreshed by the commit.
        hw_set_charger(local_param.charger_enable);
    } else {
        hw_apply_charge_limit();
    }
}

// --- Performance callbacks ---

void cpu_freq_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    uint16_t index = lv_dropdown_get_selected(obj);
    settings_commit_int("perf.cpu_mhz", kCpuFreqs[index]);
}

// --- UI style callback ---

void ui_style_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    ui_style_t st = (ui_style_t)lv_dropdown_get_selected(obj);
    const char *name = st == UI_STYLE_PIXEL_ISO ? "pixel"
                     : st == UI_STYLE_HOTEL    ? "hotel"
                     : st == UI_STYLE_BEOS     ? "beos"
                                               : "classic";
    settings_commit_str("display.ui_style", name);
    ui_style_set(st);
    core::System::getInstance().applyChromeStyle();
}

void dark_mode_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    bool on = lv_obj_has_state(obj, LV_STATE_CHECKED);
    settings_commit_bool("display.dark", on);
    ui_dark_mode_set(on);
    core::System::getInstance().applyChromeStyle();
}

} // anonymous namespace

void build_backlight(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    set_brightness_label(add_slider(sub_page, "Screen", hw_get_disp_min_brightness(),
                                    hw_get_disp_max_brightness(),
                                    local_param.brightness_level, display_brightness_cb),
                         local_param.brightness_level);

    if (hw_has_keyboard()) {
        // 0..20 slider range = 21 discrete 5% steps; each encoder tick moves 5%.
        int32_t kb_step = (int32_t)map_r(local_param.keyboard_bl_level, 0, 255, 0, 20);
        set_pct_label(add_slider(sub_page, "Keyboard", 0, 20, kb_step, keyboard_brightness_cb),
                      (long)kb_step * 5);
    }

    set_timeout_label(add_slider(sub_page, "Timeout", 0, 180,
                                 local_param.disp_timeout_second, disp_timeout_cb),
                      local_param.disp_timeout_second);

    set_sleep_mode_label(add_slider(sub_page, "Sleep Mode", 0, 1, local_param.sleep_mode,
                                    sleep_mode_cb, false),
                         local_param.sleep_mode);

    create_toggle_btn_row(sub_page, "Show Memory", local_param.show_mem_usage,
                          u8_toggle_cb, (void *)"statusbar.show_mem");
    create_toggle_btn_row(sub_page, "Show File Count", local_param.show_file_count,
                          u8_toggle_cb, (void *)"statusbar.show_files");

    // UI style: applies to newly built screens immediately and fully on
    // leaving Settings (ui_sys_exit rebuilds the theme + chrome).
    lv_obj_t *dd = create_dropdown(sub_page, "UI Style",
                                   "Classic\nPixel Iso\nHotel\nBeOS",
                                   (uint16_t)ui_style_get(), ui_style_cb);
    size_dropdown_row(dd);
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

    set_charger_current_label(add_slider(sub_page, "Current", 1, total_charge_level,
                                         curr_charge_level, charger_current_cb),
                              local_param.charger_current);

    create_toggle_btn_row(sub_page, "Limit 80%", local_param.charge_limit_en, charge_limit_cb);
}

void build_performance(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 2, 0);

    const char *freq_options = "240 MHz (Max)\n160 MHz\n80 MHz (Power)";
    uint8_t sel_idx = 0; // Default 240
    for (uint8_t i = 0; i < sizeof(kCpuFreqs) / sizeof(kCpuFreqs[0]); i++) {
        if (kCpuFreqs[i] == local_param.cpu_freq_mhz) sel_idx = i;
    }

    lv_obj_t *dd = create_dropdown(sub_page, "CPU Clock", freq_options, sel_idx, cpu_freq_cb);
    size_dropdown_row(dd);
    register_subpage_group_obj(sub_page, dd);
}

} // namespace display_cfg
