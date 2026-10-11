/**
 * @file      ui/widgets.h
 * @brief     Themed LVGL widget factories.
 *
 * These wrap raw LVGL constructors with the project's theme (accent
 * border, font, padding). Use them instead of lv_*_create() so a theme
 * tweak doesn't require touching every screen.
 *
 * Implementations live in src/ui_tools.cpp.
 */
#pragma once

#include <lvgl.h>
#include <stdint.h>

/* Settings-style menu rows (lv_menu_cont + growing label). create_slider's
 * `cb` fires on LV_EVENT_VALUE_CHANGED. */
lv_obj_t *create_text(lv_obj_t *parent, const char *txt);
lv_obj_t *create_slider(lv_obj_t *parent, const char *txt, int32_t min, int32_t max,
                        int32_t val, lv_event_cb_t cb);
lv_obj_t *create_button(lv_obj_t *parent, const char *icon, const char *txt, lv_event_cb_t cb);
lv_obj_t *create_dropdown(lv_obj_t *parent, const char *txt, const char *options, uint8_t default_sel, lv_event_cb_t cb);

/* lv_list button laid out as [icon (20 px) | text (grows, "..."-truncated) |
 * meta]. `meta` (may be NULL for none) is a muted trailing label in
 * `meta_font`. User data, events and group membership are the caller's. */
lv_obj_t *ui_list_add_meta_row(lv_obj_t *list, const char *icon, const char *text,
                               const char *meta, const lv_font_t *meta_font);

/* Turn `btn` into a BLE-HID volume control: click (or encoder press) toggles
 * the group's editing mode, the wheel then sends vol± to the paired phone
 * (clockwise = down, matching the LilyGo encoder's feel), and click again or
 * focus loss releases it. `refresh` (may be NULL) repaints the caller's
 * active/idle look after each toggle. With `require_connected`, clicks and
 * wheel steps are ignored while no BLE host is connected. Used by the home
 * screen's volume pill and the Remote app. */
void ble_volume_btn_attach(lv_obj_t *btn, void (*refresh)(void), bool require_connected);

/* LV_EVENT_FOCUSED/DEFOCUSED handler for a row's child widget: paints the
 * parent row as focused so the whole row reads as highlighted. */
void toggle_child_focus_cb(lv_event_t *e);

/* Encoder/arrow scrolling of a vertically scrollable `obj`: LV_KEY_UP/LEFT
 * scroll toward the top, LV_KEY_DOWN/RIGHT toward the bottom, by at most
 * `step` px and never past either end. Returns true when `key` was one of
 * those four (the caller then stops the event), false for any other key.
 * Shared by the Telegram history, Journal post, SSH terminal and Chat log. */
bool ui_scroll_key_clamped(lv_obj_t *obj, uint32_t key, int32_t step,
                           lv_anim_enable_t anim);

/* Pick the LVGL battery glyph that best represents the current cell state.
 * Returns LV_SYMBOL_CHARGE while charging; otherwise quantizes percent into
 * the five LV_SYMBOL_BATTERY_* steps so the icon actually tracks the level. */
const char *ui_battery_icon(int percent, bool is_charging);

lv_obj_t *create_floating_button(lv_event_cb_t event_cb, void *user_data);
/* Full-panel lv_menu with the root back button disabled. `event_cb` (may be
 * nullptr) is attached for LV_EVENT_CLICKED. */
lv_obj_t *create_menu(lv_obj_t *parent, lv_event_cb_t event_cb);
