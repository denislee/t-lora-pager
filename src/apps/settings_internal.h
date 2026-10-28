/**
 * @file      settings_internal.h
 * @brief     Private header shared between ui_settings.cpp and the 12
 *            split-out Settings subpage files (settings_*.cpp).
 *
 * Not a public API — do NOT include from anywhere else. The settings app is
 * conceptually one module; the split keeps each subpage in its own TU so
 * ui_settings.cpp stays the page wiring only.
 *
 * Contracts:
 *  - `register_subpage_group_obj` and the row helpers
 *    (`create_toggle_btn_row`, `settings_kv_row`, `settings_action_row`)
 *    live in ui_settings.cpp with external linkage so each split-out TU can
 *    wire its widgets into the shared focus-group tracking.
 *  - Each sub-config namespace exposes `build_subpage()` (entry point listed
 *    in ui_settings.cpp's kTiles table and stored on the LVGL page's
 *    user_data), optionally `set_sub_page()` (also listed in kTiles, for
 *    subpages that rebuild themselves in place), and `reset_state()`
 *    (called from ui_sys_exit to clear cached LVGL pointers before the
 *    menu is deleted).
 *  - The `weather_*` and `timezone_*` forward declarations refer to symbols
 *    defined in ui_weather.cpp / ui_time_sync.cpp — those modules predate
 *    the split and still own the storage and geocoding logic.
 */
#pragma once

#include <lvgl.h>
#include <string>
#include <vector>

#include "../hal_interface.h"  // user_setting_params_t
#include "../hal/settings.h"

// --- Shared helpers implemented in ui_settings.cpp ---
void register_subpage_group_obj(lv_obj_t *page, lv_obj_t *obj);
// "label ...... [ On ]" row on `page`. The checkable button is registered
// with `page`'s focus group, `cb` fires on LV_EVENT_VALUE_CHANGED with
// `user_data` as the event user_data, and the button label flips between
// on_txt/off_txt by itself (after `cb`, so a cb that clears
// LV_STATE_CHECKED to reject the change shows off_txt). Returns the button.
lv_obj_t *create_toggle_btn_row(lv_obj_t *page, const char *txt,
                                bool initial_state, lv_event_cb_t cb,
                                void *user_data = nullptr,
                                const char *on_txt = " On ",
                                const char *off_txt = " Off ");
// Read-only "key ........ value" row (muted key, value truncated with an
// ellipsis past val_max_pct of the row), focusable so the encoder can
// scroll the page. Registered with `page`'s focus group. Returns the value
// label so callers can refresh it.
lv_obj_t *settings_kv_row(lv_obj_t *page, const char *key, const char *val,
                          int32_t pad_ver = 4, int32_t val_max_pct = 55);
// "label ...... [btn_txt]" row whose button fires `cb` on LV_EVENT_CLICKED.
// Registered with `page`'s focus group. Returns the button.
lv_obj_t *settings_action_row(lv_obj_t *page, const char *txt, const char *btn_txt,
                              lv_event_cb_t cb, int32_t btn_width = 60);
// Muted, wrapping, full-width label (status / hint lines on a subpage).
lv_obj_t *settings_muted_label(lv_obj_t *parent);
// create_button row (icon + label, fires `cb` on click) registered with
// `page`'s focus group. Returns the clickable button.
lv_obj_t *settings_button_row(lv_obj_t *page, const char *icon, const char *txt,
                              lv_event_cb_t cb);
// Token row + notes-crypto note shared by the Telegram and Notes Sync
// subpages. `display` is the masked token (hal::secret_mask_display) and
// `encrypted` whether ciphertext is stored; the note (hardware only) says
// what notes_crypto needs before the token can be saved or read, with
// `disabled_note` / `ready_note` as the subpage's wording for the
// encryption-off and unlocked states. Either label may be null.
void settings_refresh_token_labels(lv_obj_t *tok_label, lv_obj_t *note_label,
                                   const std::string &display, bool encrypted,
                                   const char *disabled_note,
                                   const char *ready_note);
// Re-populate the Settings focus group with the subpage's registered widgets
// (skipping hidden ones). Used by subpages that toggle row visibility and
// need the keyboard focus list rebuilt in-place (connectivity's WiFi/NFC
// Test rows follow their toggle's state).
void activate_subpage_group(lv_obj_t *page);
// LV_EVENT_KEY | LV_EVENT_PREPROCESS handler that swaps LV_KEY_LEFT and
// LV_KEY_RIGHT when the input comes from the encoder, so rotating the
// wheel moves a spinbox/slider in the visually expected direction. Other
// keys and other input devices pass through untouched. Used by datetime,
// backlight, and charger subpages.
void invert_scroll_key_cb(lv_event_t *e);
// Pop the Settings menu back to the root page (e.g. datetime "Save" does
// this after applying the entered time).
void settings_return_to_main_page();
// toggle_child_focus_cb (row focus mirroring for custom row buttons, used by
// the toggle/action rows and sliders) is declared in ui/widgets.h.
// Drop any subpage_items entries for `page` from the focus-group tracking.
// Notes Security calls this when rebuilding its page in place after a
// passphrase op, so activate_subpage_group won't re-add freed widgets.
void unregister_subpage_items_for(lv_obj_t *page);

// Read-only snapshot of the user settings blob, used to build the subpages.
// Refreshed from the HAL on entry, after every settings_commit*() and when
// the phone changes a setting. Never written back: every control persists
// its own setting through the registry (hal/settings.h), so a value the
// phone changes while Settings is open is not clobbered on exit (T1).
extern user_setting_params_t local_param;

// --- Write-through to the settings registry (IOS_SETTINGS_PLAN.md W3) ---
// Store one setting from a Settings control. `apply` false (the default):
// the control has already applied the change itself, so only store + notify
// the phone. Call it BEFORE a HAL setter that also mutates the live settings
// struct, or the registry sees "unchanged" and skips the write. On failure
// `msg` gets the reason.
bool settings_commit(const char *id, const settings::Value &v, bool apply = false,
                     std::string *msg = nullptr);
inline bool settings_commit_bool(const char *id, bool v) { return settings_commit(id, settings::Value::of_bool(v)); }
inline bool settings_commit_int(const char *id, int64_t v) { return settings_commit(id, settings::Value::of_int(v)); }
inline bool settings_commit_str(const char *id, const std::string &v) { return settings_commit(id, settings::Value::of_str(v)); }
// Slider notches: store only the last value within ~400 ms (store-only, like
// settings_commit with apply false). ui_sys_exit flushes what is pending.
void settings_commit_later(const char *id, const settings::Value &v);
void settings_flush_pending();

// --- Timezone (timezone_get_user_tz/set_user_tz/fetch_list) ---
#include "../hal/timezone.h"

// --- Weather geocoding: defined in ui_weather.cpp ---
struct weather_city_match {
    std::string label;  // "Paris, Île-de-France, France" — shown in the list
    std::string name;   // "Paris" — stored as user_city
    double lat;
    double lon;
};
std::string weather_get_user_city();
void        weather_set_user_city(const char *city);
void        weather_set_user_location(const char *city, double lat, double lon);
bool        weather_search_cities(const char *query,
                                  std::vector<weather_city_match> &out,
                                  std::string &err);
// --- Per-sub-config entry points ---
namespace weather_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// Local Hub subpage — master toggle + URL for the lilyhub server. Backed by
// hal::hub_*. Cross-cutting: weather and notes-sync both consult this.
namespace hub_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

namespace telegram_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

namespace notes_sync_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// Fonts subpage has no persistent state of its own — its 18 dropdowns (9
// contexts × face/size) share two callbacks that commit the registry id
// (fonts.<ctx>.face / .size) passed as user_data. No set_sub_page/
// reset_state needed.
namespace fonts_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
}

// Datetime subpage has its own state (spinbox pointers + NTP sync timer).
// reset_state() must run from ui_sys_exit so the poll timer doesn't outlive
// the page it drives and touch a deleted status label.
namespace datetime_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// System Info subpage — periodic 1 Hz label refresh (clock, RSSI, storage).
// reset_state() kills the lv_timer so it doesn't tick after the page is
// gone.
namespace info_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// Connectivity subpage — WiFi/BT/Radio/NFC/Speaker/Haptic toggles +
// "WiFi Networks", "Test Internet", "NFC Test" buttons (whose visibility
// follows the corresponding toggle's state). reset_state() nulls cached
// widget pointers so a subsequent ui_sys_enter doesn't touch stale LVGL
// objects.
namespace connectivity_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}

// Display & Backlight / Charger / Performance subpages. Bundled because
// each is small on its own and they share the same dependency surface
// (local_param + invert_scroll_key_cb + toggle rows). No cached state.
namespace display_cfg {
    void build_backlight(lv_obj_t *menu, lv_obj_t *sub_page);
    void build_otg(lv_obj_t *menu, lv_obj_t *sub_page);
    void build_performance(lv_obj_t *menu, lv_obj_t *sub_page);
}

// Storage subpage — USB MSC target, pruning toggle + manual actions.
// No cached LVGL state, so no reset_state.
namespace storage_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
}

// Notes Security subpage — encryption-at-rest for *.txt notes. Bundled
// with storage because both share the storage_loader popup triad used
// for long-running filesystem ops. g_sub_page is cached so a passphrase
// op can rebuild the page in place with the fresh Status line.
namespace notes_sec_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void set_sub_page(lv_obj_t *page);
    void reset_state();
}

// Home Apps subpage — per-tile visibility toggles for the home screen
// grid. Reads/writes through apps::home_apps_is_visible / _set_visible
// (NVS-backed). No cached LVGL state.
namespace home_apps_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
}

// IMU Debug subpage — read-only diagnostic view of the BHI260 (or BMA423)
// state. Shows which virtual sensors the loaded firmware advertises,
// configure() success per sensor, live event counters, current accel /
// roll / pitch / device-orientation values, and a "Re-init IMU" button to
// retry registration without rebooting. A 200 ms lv_timer drives the
// live rows; reset_state() kills the timer when the page is torn down.
namespace imu_debug_cfg {
    void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page);
    void reset_state();
}
