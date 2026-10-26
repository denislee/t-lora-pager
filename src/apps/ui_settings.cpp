/**
 * @file      ui_settings.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2025  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2025-01-05
 *
 * State reset on onStop (ui_sys_exit):
 *   widgets: menu, quit_btn            — destroyed via lv_obj_del then nulled
 *            settings_main_page, settings_exit_btn
 *                                       — nulled (deleted with the menu /
 *                                         owned by the status bar)
 *   arrays:  main_page_group_items[], subpage_items[], settings_shortcuts[]
 *                                       — cleared, counts reset to 0
 *   group:   s_group                   — not owned (the app group); kept
 *   settings: local_param               — flushed to NVS (hw_set_user_setting),
 *                                         then theme_init() rebuilds the theme
 *                                         (and the chrome) if a style/font changed
 *   subpages: ui_sys_exit calls each subpage namespace's reset_state() by
 *             hand (info, datetime, connectivity, notes_sec, hub, weather,
 *             telegram, notes_sync, imu_debug) to null its cached state. The
 *             display, fonts, storage and home_apps subpages cache no LVGL
 *             pointers and have none.
 * Each settings_X.cpp is responsible for its own subpage state. If you add a
 * subpage TU that caches lv_obj_t* / timers, give it a reset_state() and add
 * the call to ui_sys_exit — otherwise its cached pointers will dangle.
 */
#include "../ui_define.h"
#include "../hal/notes_crypto.h"
#include "../core/app_manager.h"
#include "../core/system.h"
#include "../core/input_profile.h"
#include "app_registry.h"
#include "settings_internal.h"

using std::string;

// weather_city_match / weather_*  and  timezone_*  forward decls all live
// in apps/settings_internal.h (shared with the split-out settings_*.cpp).

static void style_menu_item_icon(lv_obj_t *cont, const char *icon, const char *text)
{
    lv_obj_t *icon_label = lv_label_create(cont);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_min_width(icon_label, 20, 0);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *text_label = lv_label_create(cont);
    lv_label_set_text(text_label, text);
}

static lv_obj_t *menu = NULL;
// External linkage — shared with the split-out settings_*.cpp files via
// apps/settings_internal.h. Not strictly a global API; treat as private.
user_setting_params_t local_param;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *settings_main_page = NULL;
static lv_obj_t *settings_exit_btn = NULL;
// The focus group the settings widgets live in (the app group, IS.4) —
// captured from lv_group_get_default() on entry.
static lv_group_t *s_group = NULL;

#define MAX_MAIN_PAGE_ITEMS 24
static lv_obj_t *main_page_group_items[MAX_MAIN_PAGE_ITEMS];
static uint8_t main_page_group_count = 0;

#define MAX_SUBPAGE_ITEMS 64
static struct {
    lv_obj_t *page;
    lv_obj_t *obj;
} subpage_items[MAX_SUBPAGE_ITEMS];
static uint8_t subpage_item_count = 0;

static void add_main_page_group_item(lv_obj_t *obj)
{
    if (main_page_group_count < MAX_MAIN_PAGE_ITEMS) {
        main_page_group_items[main_page_group_count++] = obj;
    }
    lv_group_add_obj(s_group, obj);
}

// --- Main-page keyboard shortcuts ---------------------------------------
// Every grid tile gets a single-letter shortcut: a key-cap badge is drawn on
// the tile (so it's self-documenting) and the key is wired so pressing it on
// the physical keyboard focuses + activates that tile. LV_EVENT_KEY only
// fires on the *focused* widget, so the handler is attached to every tile
// (and the shared back button); whichever holds focus catches the key and
// dispatches to the mapped target. Mirrors the home-screen shortcut
// convention in menu_app.cpp. Only active on the root page — the handler
// bails when a subpage is open so letters typed into subpage fields (or a
// focused back button on a subpage) never trigger a jump.
#define MAX_SETTINGS_SHORTCUTS 32
static struct {
    char key;            // uppercase ASCII
    lv_obj_t *target;    // tile to activate when the key is pressed
} settings_shortcuts[MAX_SETTINGS_SHORTCUTS];
static uint8_t settings_shortcut_count = 0;

static char shortcut_upper(uint32_t k)
{
    return (k >= 'a' && k <= 'z') ? (char)(k - 'a' + 'A') : (char)k;
}

static void settings_shortcut_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    // Root page only: on a subpage the tiles aren't focused anyway, but the
    // shared back button is — guard so its key events don't jump pages.
    if (!menu || lv_menu_get_cur_main_page(menu) != settings_main_page) return;
    char key = shortcut_upper(lv_event_get_key(e));
    for (uint8_t i = 0; i < settings_shortcut_count; i++) {
        if (settings_shortcuts[i].key == key && settings_shortcuts[i].target) {
            lv_obj_t *t = settings_shortcuts[i].target;
            lv_group_focus_obj(t);
            lv_obj_send_event(t, LV_EVENT_CLICKED, NULL);
            return;
        }
    }
}

// Draw the small key-cap badge on a tile, flush to the right edge. The tile
// is a flex row [icon | text]; letting the text label take the slack pushes
// the badge to the far right, vertically centered.
static void add_shortcut_badge(lv_obj_t *cont, char key)
{
    lv_obj_t *text_label = lv_obj_get_child(cont, 1); // 0 = icon, 1 = text
    if (text_label) lv_obj_set_flex_grow(text_label, 1);

    lv_obj_t *badge = lv_label_create(cont);
    char txt[2] = { key, '\0' };
    lv_label_set_text(badge, txt);
    lv_obj_set_style_text_font(badge, get_small_font(), 0);
    lv_obj_set_style_text_color(badge, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(badge, UI_COLOR_MUTED, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_set_style_radius(badge, 4, 0);
    lv_obj_set_style_pad_hor(badge, 4, 0);
    lv_obj_set_style_pad_ver(badge, 1, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_50, 0);
}

// Attach a shortcut to a freshly created tile: record key→tile, wire the key
// handler, and draw the badge.
static void settings_register_shortcut(lv_obj_t *cont, char key)
{
    if (!cont) return;
    if (settings_shortcut_count < MAX_SETTINGS_SHORTCUTS) {
        settings_shortcuts[settings_shortcut_count].key = key;
        settings_shortcuts[settings_shortcut_count].target = cont;
        settings_shortcut_count++;
    }
    lv_obj_add_event_cb(cont, settings_shortcut_key_cb, LV_EVENT_KEY, NULL);
    add_shortcut_badge(cont, key);
}

// External linkage — called from the split-out settings_*.cpp files. See
// apps/settings_internal.h.
void register_subpage_group_obj(lv_obj_t *page, lv_obj_t *obj)
{
    if (subpage_item_count < MAX_SUBPAGE_ITEMS) {
        subpage_items[subpage_item_count].page = page;
        subpage_items[subpage_item_count].obj = obj;
        subpage_item_count++;
    }
}

// External linkage — swap-remove any entries tracking widgets on `page`.
// Used by notes_sec_cfg::mark_for_rebuild before lv_obj_clean, so
// activate_subpage_group won't re-add freed widgets on the next focus.
void unregister_subpage_items_for(lv_obj_t *page)
{
    for (uint8_t i = 0; i < subpage_item_count; ) {
        if (subpage_items[i].page == page) {
            subpage_items[i] = subpage_items[--subpage_item_count];
        } else {
            i++;
        }
    }
}

static void restore_main_page_group()
{
    lv_group_remove_all_objs(s_group);
    if (settings_exit_btn) {
        lv_group_add_obj(s_group, settings_exit_btn);
    }
    for (uint8_t i = 0; i < main_page_group_count; i++) {
        lv_group_add_obj(s_group, main_page_group_items[i]);
    }
    if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }
}

// External linkage — shared with the split-out settings_*.cpp files.
void activate_subpage_group(lv_obj_t *page)
{
    lv_group_remove_all_objs(s_group);
    // Status bar back is the visible "<" on subpages — its callback pops to
    // root. Keep it keyboard-navigable and initially focused.
    if (settings_exit_btn) {
        lv_group_add_obj(s_group, settings_exit_btn);
    }
    for (uint8_t i = 0; i < subpage_item_count; i++) {
        if (subpage_items[i].page == page &&
            !lv_obj_has_flag(subpage_items[i].obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_group_add_obj(s_group, subpage_items[i].obj);
        }
    }
    if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }
}

// External linkage — shared with the split-out settings_*.cpp files.
void invert_scroll_key_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;
    uint32_t *key = (uint32_t *)lv_event_get_param(e);
    if (!key) return;
    switch (*key) {
    case LV_KEY_LEFT:  *key = LV_KEY_RIGHT; break;
    case LV_KEY_RIGHT: *key = LV_KEY_LEFT;  break;
    }
}

// Helper used by split-out subpages (e.g. datetime's "Apply") that need to
// pop the user back to the Settings root. Declared in apps/settings_internal.h.
void settings_return_to_main_page()
{
    if (menu && settings_main_page) {
        lv_menu_clear_history(menu);
        lv_menu_set_page(menu, settings_main_page);
    }
}

static void build_device_probe(lv_obj_t *menu, lv_obj_t *sub_page)
{
    lv_obj_add_flag(sub_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 4, 0);
    lv_obj_set_style_pad_row(sub_page, 0, 0);

    uint16_t count = 0;
    const hw_device_t *devices = hw_get_devices(count);
    uint32_t devices_mask = hw_get_device_online();
    for (uint16_t i = 0; i < count; ++i) {
        bool online = (devices_mask & devices[i].online_bit) != 0;

        lv_obj_t *status = settings_kv_row(sub_page, devices[i].name,
                                           online ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, 5);
        lv_obj_set_style_text_color(status,
            lv_palette_main(online ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
        // Here the key (device name) is the primary text: full colour,
        // truncated rather than the one-glyph status.
        lv_obj_t *name = lv_obj_get_child(lv_obj_get_parent(status), 0);
        lv_obj_remove_local_style_prop(name, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_max_width(name, LV_PCT(65), 0);
    }
}

// --- Main-page tile table ------------------------------------------------
// One row per grid tile, in display (and focus) order. A tile either opens a
// subpage (`builder` set — lv_menu loads the page and settings_page_changed_cb
// runs the builder the first time it is shown) or switches to another app
// (`app` set). `set_sub_page` is for subpages that rebuild themselves in place.
typedef void (*subpage_builder_t)(lv_obj_t *menu, lv_obj_t *page);

struct SettingsTile {
    const char *icon;
    const char *label;
    char key;                              // keyboard shortcut, see below
    subpage_builder_t builder;             // subpage tile, or NULL
    void (*set_sub_page)(lv_obj_t *page);  // optional, subpage tiles only
    const char *app;                       // app-shortcut tile, or NULL
};

// Keys are mnemonic first letters where free, disambiguated on collision
// (see the badge on each tile for the exact key). Keep them unique; 'X' is
// taken by the Power Off tile appended after the table.
static const SettingsTile kTiles[] = {
    // Appearance
    { LV_SYMBOL_IMAGE,     "Display & Backlight", 'D', display_cfg::build_backlight,    NULL, NULL },
    { LV_SYMBOL_EDIT,      "Fonts",               'F', fonts_cfg::build_subpage,        NULL, NULL },
    // Time & power
    { LV_SYMBOL_REFRESH,   "Date & Time",         'T', datetime_cfg::build_subpage,     NULL, NULL },
    { LV_SYMBOL_CHARGE,    "Charger",             'C', display_cfg::build_otg,          NULL, NULL },
    // Connectivity & hardware
    { LV_SYMBOL_WIFI,      "Connectivity",        'N', connectivity_cfg::build_subpage, NULL, NULL }, // Network
    { LV_SYMBOL_USB,       "Devices",             'V', build_device_probe,              NULL, NULL }, // deVices
    // Home screen
    { LV_SYMBOL_HOME,      "Home Apps",           'H', home_apps_cfg::build_subpage,    NULL, NULL },
    // Online services
    { LV_SYMBOL_UPLOAD,    "Local Hub",           'L', hub_cfg::build_subpage,          NULL, NULL },
    { LV_SYMBOL_REFRESH,   "Weather",             'W', weather_cfg::build_subpage,      NULL, NULL },
    { LV_SYMBOL_ENVELOPE,  "Telegram",            'G', telegram_cfg::build_subpage,     NULL, NULL }, // teleGram
    // Notes
    { LV_SYMBOL_REFRESH,   "Notes Sync",          'Y', notes_sync_cfg::build_subpage,   NULL, NULL }, // sYnc
    { LV_SYMBOL_EYE_CLOSE, "Notes Security",      'E', notes_sec_cfg::build_subpage,    notes_sec_cfg::set_sub_page, NULL }, // sEcurity
    // Storage & files
    { LV_SYMBOL_SD_CARD,   "Storage",             'S', storage_cfg::build_subpage,      NULL, NULL },
    { LV_SYMBOL_DIRECTORY, "Files",               'I', NULL, NULL, "Files" },      // fIles
    // App shortcuts
    { LV_SYMBOL_AUDIO,     "Recordings",          'R', NULL, NULL, "Recordings" },
    { LV_SYMBOL_BLUETOOTH, "Remote",              'M', NULL, NULL, "Remote" },     // reMote
    // System
    { LV_SYMBOL_SETTINGS,  "Performance",         'P', display_cfg::build_performance,  NULL, NULL },
    { LV_SYMBOL_LIST,      "System Info",         'O', info_cfg::build_subpage,         NULL, NULL }, // infO
    { LV_SYMBOL_EYE_OPEN,  "IMU Debug",           'U', imu_debug_cfg::build_subpage,    NULL, NULL }, // imU
};

// App-shortcut tiles. Defer the app switch via queueSwitchApp: ui_sys_exit()
// deletes the settings `menu`, which is an ancestor of the clicked menu item
// whose CLICKED event is still being dispatched. Freeing it synchronously
// leaves LVGL walking freed memory and freezes the device. queueSwitchApp
// runs from AppManager::update() after lv_timer_handler fully unwinds —
// guaranteed next-main-loop-iteration, unlike lv_async_call (period-0 timer)
// which races the current event dispatch and could miss the first click.
static void tile_app_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const SettingsTile *t = (const SettingsTile *)lv_event_get_user_data(e);
    hw_feedback();
    core::AppManager::getInstance().queueSwitchApp(t->app,
        core::System::getInstance().getAppPanel());
}

static lv_obj_t *create_tile(lv_obj_t *menu, lv_obj_t *main_page, const SettingsTile &t)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    style_menu_item_icon(cont, t.icon, t.label);
    if (t.builder) {
        lv_obj_t *sub_page = lv_menu_page_create(menu, NULL);
        lv_obj_set_user_data(sub_page, (void *)t.builder);
        lv_menu_set_load_page_event(menu, cont, sub_page);
        if (t.set_sub_page) t.set_sub_page(sub_page);
    } else {
        lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(cont, tile_app_click_cb, LV_EVENT_CLICKED, (void *)&t);
    }
    return cont;
}

static void power_off_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    lv_delay_ms(200);
    hw_shutdown();
}

static lv_obj_t *create_power_off_item(lv_obj_t *main_page)
{
    lv_obj_t *cont = lv_menu_cont_create(main_page);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    style_menu_item_icon(cont, LV_SYMBOL_POWER, "Power Off");
    lv_obj_t *icon_label = lv_obj_get_child(cont, 0);
    if (icon_label) {
        lv_obj_set_style_text_color(icon_label,
                                    lv_palette_main(LV_PALETTE_RED), 0);
    }
    lv_obj_add_event_cb(cont, power_off_click_cb, LV_EVENT_CLICKED, NULL);
    return cont;
}

static void settings_page_changed_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    // Ignore events bubbling from children (like dropdowns)
    if (lv_event_get_target(e) != obj) return;

    lv_obj_t *page = lv_menu_get_cur_main_page(obj);
    if (page != settings_main_page) {
        subpage_builder_t builder = (subpage_builder_t)lv_obj_get_user_data(page);
        if (builder) {
            builder(menu, page);
            lv_obj_set_user_data(page, NULL); // Only build once
        }
        activate_subpage_group(page);
    } else {
        restore_main_page_group();
    }
}

static void settings_exit_cb(lv_event_t *e)
{
    // On the root settings page the status bar back exits the app entirely.
    // On a subpage it pops back to the root page. Forwarding a click to the
    // built-in back button proved unreliable (the header back is hidden/
    // zero-sized, and the event doesn't always drive lv_menu's pop), so
    // clear history and re-load the root page directly.
    if (menu) {
        lv_obj_t *page = lv_menu_get_cur_main_page(menu);
        if (page && page != settings_main_page) {
            lv_menu_clear_history(menu);
            lv_menu_set_page(menu, settings_main_page);
            return;
        }
    }
    // menu_show_deferred will trigger AppManager::switchApp which calls ui_sys_exit
    menu_show_deferred();
}

// Flips a toggle row's label to match the button's *final* checked state.
// Registered after the caller's cb, so a cb that rejects the change by
// clearing LV_STATE_CHECKED (e.g. OTG) is reflected too. The on-text rides
// as this event's user_data, the off-text as the label's own user_data.
static void toggle_label_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *label = (lv_obj_t *)lv_obj_get_user_data(btn);
    if (!label) return;
    lv_label_set_text(label, lv_obj_has_state(btn, LV_STATE_CHECKED)
                                 ? (const char *)lv_event_get_user_data(e)
                                 : (const char *)lv_obj_get_user_data(label));
}

lv_obj_t *create_toggle_btn_row(lv_obj_t *page, const char *txt, bool initial_state,
                                lv_event_cb_t cb, void *user_data,
                                const char *on_txt, const char *off_txt)
{
    lv_obj_t *obj = create_text(page, txt);

    lv_obj_t *btn = lv_btn_create(obj);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE); 
    if (initial_state) lv_obj_add_state(btn, LV_STATE_CHECKED);
    
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_CHECKED);
    
    // Fix width to align "On/Off" texts to the right, like the standard labels
    lv_obj_set_width(btn, 60);
    lv_obj_set_flex_grow(btn, 0);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, initial_state ? on_txt : off_txt);
    lv_obj_center(label);

    lv_obj_set_user_data(btn, label);
    lv_obj_set_user_data(label, (void *)off_txt);

    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_VALUE_CHANGED, user_data);
    lv_obj_add_event_cb(btn, toggle_label_cb, LV_EVENT_VALUE_CHANGED, (void *)on_txt);
    register_subpage_group_obj(page, btn);

    return btn;
}

lv_obj_t *settings_kv_row(lv_obj_t *page, const char *key, const char *val,
                          int32_t pad_ver, int32_t val_max_pct)
{
    lv_obj_t *row = lv_menu_cont_create(page);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, pad_ver, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_mix(UI_COLOR_FG, UI_COLOR_BG, LV_OPA_30), 0);
    lv_obj_set_style_radius(row, 0, 0);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, UI_COLOR_MUTED, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, LV_PCT(val_max_pct), 0);

    register_subpage_group_obj(page, row);
    return v;
}

void settings_refresh_token_labels(lv_obj_t *tok_label, lv_obj_t *note_label,
                                   const std::string &display, bool encrypted,
                                   const char *disabled_note,
                                   const char *ready_note)
{
    if (tok_label) {
        // Tokens are only ever stored encrypted, so a non-encrypted slot
        // means "no token" — the display string already says "(not set)"
        // then, and the suffix only marks a stored token.
        lv_label_set_text_fmt(tok_label, "Token: %s%s", display.c_str(),
                              encrypted ? "  [encrypted]" : "");
    }
#ifdef ARDUINO
    if (note_label) {
        const char *note;
        if (!notes_crypto_is_enabled()) {
            note = disabled_note;
        } else if (!notes_crypto_is_unlocked()) {
            note = "Notes session locked — unlock (open Notes) to "
                   "save or read the token.";
        } else {
            note = ready_note;
        }
        lv_label_set_text(note_label, note);
    }
#else
    (void)note_label; (void)disabled_note; (void)ready_note;
#endif
}

lv_obj_t *settings_muted_label(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_color(l, UI_COLOR_MUTED, 0);
    return l;
}

lv_obj_t *settings_button_row(lv_obj_t *page, const char *icon, const char *txt,
                              lv_event_cb_t cb)
{
    lv_obj_t *btn = create_button(page, icon, txt, cb);
    register_subpage_group_obj(page, btn);
    return btn;
}

lv_obj_t *settings_action_row(lv_obj_t *page, const char *txt, const char *btn_txt,
                              lv_event_cb_t cb, int32_t btn_width)
{
    lv_obj_t *row = create_text(page, txt);

    lv_obj_t *btn = lv_btn_create(row);
    lv_obj_set_width(btn, btn_width);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_shadow_width(btn, 0, LV_STATE_FOCUS_KEY);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, btn_txt);
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, toggle_child_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    register_subpage_group_obj(page, btn);
    return btn;
}

static void ui_sys_enter(lv_obj_t *parent)
{
    INPUT_PROF_SPAN("ui_sys_enter");
    if (menu != NULL) return;
    s_group = lv_group_get_default();

    enable_keyboard();

    hw_get_user_setting(local_param);

    menu = create_menu(parent);

    // Suppress the menu's built-in header back button: LVGL re-un-hides it on
    // each subpage, so a plain HIDDEN flag on the header doesn't stick. Zero
    // its size and styling so the header's content_height stays 0 and LVGL's
    // own refr_main_header_mode hides the whole header automatically. It
    // remains programmatically clickable for the status bar back to drive.
    lv_obj_t *bb = lv_menu_get_main_header_back_button(menu);
    if (bb) {
        lv_obj_set_size(bb, 0, 0);
        lv_obj_set_style_pad_all(bb, 0, 0);
        lv_obj_set_style_border_width(bb, 0, 0);
        lv_obj_set_style_outline_width(bb, 0, 0);
        lv_obj_set_style_shadow_width(bb, 0, 0);
        lv_obj_set_style_bg_opa(bb, LV_OPA_TRANSP, 0);
    }

    /*Create a main page*/
    lv_obj_t *main_page = lv_menu_page_create(menu, NULL);

    lv_obj_t *cont;
    main_page_group_count = 0;
    subpage_item_count = 0;

    // Flatten the item list into a two-column grid: each lv_menu_cont gets
    // LV_PCT(48) width and the page wraps rows of two.
    lv_obj_set_flex_flow(main_page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(main_page, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(main_page, 6, 0);
    lv_obj_set_style_pad_row(main_page, 6, 0);

    // Grid tile with a single-letter keyboard shortcut (badge + key wiring).
    // The keys live in kTiles above.
    auto add_grid_item_sc = [&](lv_obj_t *c, char key) {
        if (!c) return;
        lv_obj_set_width(c, LV_PCT(48));
        add_main_page_group_item(c);
        settings_register_shortcut(c, key);
    };

    // Back button lives on the status bar. Keep the pointer so
    // restore_main_page_group() can focus it when returning from a subpage.
    settings_exit_btn = ui_show_back_button(settings_exit_cb);
    // So a shortcut still fires when focus sits on the back button (the
    // initial focus after popping back from a subpage). The handler's
    // root-page guard keeps it inert while a subpage is open. Cleaned up by
    // ui_hide_back_button() in ui_sys_exit (it clears all back-button cbs).
    if (settings_exit_btn) {
        lv_obj_add_event_cb(settings_exit_btn, settings_shortcut_key_cb,
                            LV_EVENT_KEY, NULL);
    }

    for (const SettingsTile &t : kTiles) {
        cont = create_tile(menu, main_page, t);
        add_grid_item_sc(cont, t.key);
    }
    cont = create_power_off_item(main_page);
    add_grid_item_sc(cont, 'X'); // eXit/off

    settings_main_page = main_page;
    lv_menu_set_page(menu, main_page);
    lv_obj_add_event_cb(menu, settings_page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Without this the encoder has no focus anchor on the very first entry
    // (the menu tiles that used to hold focus were just cleaned up by
    // switchApp), so rotation does nothing until something is touched.
    if (main_page_group_count > 0) {
        lv_group_focus_obj(main_page_group_items[0]);
    } else if (settings_exit_btn) {
        lv_group_focus_obj(settings_exit_btn);
    }

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button(settings_exit_cb, NULL);
#endif
}


static void ui_sys_exit()
{
    info_cfg::reset_state();
    ui_hide_back_button();
    // Intentionally do NOT disable_keyboard() here: the next app (e.g. Files
    // browser) immediately calls enable_keyboard(), which on T-LoRa-Pager
    // cycles the TCA8418 (kb.end()/kb.begin() → detach/reattach ISR + ledc).
    // That cycle races with the core-0 keyboard_task polling the same chip
    // and hangs the device. Keyboard is a system-level resource; leave it on
    // across app switches. Sleep path (ui_pause_timers) still disables it.
    ui_app_widgets_del(&menu, &quit_btn);
    hw_set_user_setting(local_param);
    // Rebuild the theme so any system-font change takes effect on the next
    // screen that gets constructed. Objects already created keep their old
    // font, but the next UI (menu, app, dialog) picks up the new one.
    // theme_init is a no-op when nothing changed (P7.11); style and dark-mode
    // changes already re-tinted the chrome from their own callbacks.
    if (theme_init()) core::System::getInstance().applyChromeStyle();
    settings_main_page = NULL;
    settings_exit_btn = NULL;
    // Drop any in-flight NTP poll timer so it doesn't outlive the page it
    // was driving and touch a deleted status label.
    datetime_cfg::reset_state();
    connectivity_cfg::reset_state();
    notes_sec_cfg::reset_state();
    hub_cfg::reset_state();
    weather_cfg::reset_state();
    telegram_cfg::reset_state();
    notes_sync_cfg::reset_state();
    imu_debug_cfg::reset_state();
    main_page_group_count = 0;
    subpage_item_count = 0;
    settings_shortcut_count = 0;
    memset(main_page_group_items, 0, sizeof(main_page_group_items));
    memset(subpage_items, 0, sizeof(subpage_items));
    memset(settings_shortcuts, 0, sizeof(settings_shortcuts));
}

namespace {
class SysApp : public core::App {
public:
    SysApp() : core::App("Settings") {}
    void onStart(lv_obj_t *parent) override { ui_sys_enter(parent); }
    void onStop() override {
        ui_sys_exit();
        core::App::onStop();
    }
};
} // namespace

namespace apps {
APP_FACTORY(make_sys_app, SysApp)
} // namespace apps
