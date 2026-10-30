/**
 * @file      menu_app.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 *
 * Home screen. Layout, top-to-bottom:
 *   1. Quick-toggles strip (media pills + WiFi/BT + Settings), right-aligned.
 *   2. App-tile grid, ordered by activity:
 *        Create → Communicate → Info → System.
 *
 * IS.3: the screen is built once and kept alive while apps run; onStop only
 * pauses the live timers and onStart either resumes or (layout changed)
 * rebuilds.
 *
 * The grid uses a darkened resting state with a subtle accent border so tiles
 * carry their identity even when unfocused, and a tinted fill + outer accent
 * halo on focus so the selected tile reads at a glance from across the room.
 */
#include "menu_app.h"
#include "../core/system.h"
#include "../core/one_shot_job.h"
#include "../core/input_profile.h"
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../hal/link.h"
#include "../hal/hub.h"
#include "../hal/nvs.h"
#include "app_registry.h"
#include <vector>
#include <string>
#ifdef ARDUINO
#include <Preferences.h>
#endif

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(font_montserrat_48_subset);  // P7.51 src/fonts/font_montserrat_48_subset.c

namespace apps {

namespace {

// ---- App tiles -----------------------------------------------------------

// A badge function returns the small unread/notification count to render in
// the tile corner. Return 0 to hide the badge. Null means the tile never has
// a badge.
using BadgeFn = int (*)();

struct HomeItem {
    const char* label;
    const char* symbol;
    const char* appName;
    BadgeFn badge_fn;
    bool requires_wifi;
    // also usable over the phone link (PagerLink)
    bool phone_ok;
};

// Order groups apps by what the user is doing:
//   [create/capture]        Notes, Tasks, Recorder
//   [communicate]           Telegram, Chat, Inbox (phone messages)
//   [consume/check]         Weather
//   [system]                SSH
// Settings lives in the top toggle strip as a small icon rather than the grid.
//
// All tiles share the global accent color so the grid reads as a single
// uniform surface — identity comes from the icon and label, not from hue.
static const HomeItem kItems[] = {
    {"Notes",    LV_SYMBOL_EDIT,      "Editor",   nullptr,              false, false},
    {"Tasks",    LV_SYMBOL_OK,        "Tasks",    nullptr,              false, false},
    {"Recorder", LV_SYMBOL_AUDIO,     "Recorder", nullptr,              false, false},
    {"Telegram", LV_SYMBOL_ENVELOPE,  "Telegram", tg_get_unread_count,  true,  true },
    {"Chat",     LV_SYMBOL_BELL,      "Chat",     nullptr,              true,  true },
    {"Inbox",    LV_SYMBOL_DOWNLOAD,  "Inbox",    inbox_unread_count,   false, false},
    {"Weather",  LV_SYMBOL_TINT,      "Weather",  nullptr,              false, false},
    {"SSH",      LV_SYMBOL_KEYBOARD,  "SSH",      nullptr,              true,  false},
};
constexpr int kItemCount = sizeof(kItems) / sizeof(kItems[0]);

// Keeps the badge labels alive for the lifetime of the menu view so the
// periodic timer can refresh them. Reset each time the home screen is rebuilt.
struct Badge {
    lv_obj_t* label;
    BadgeFn   fn;
    // P4.13a: cached count — lv_label_set_text in LVGL 9 calls
    // lv_obj_invalidate() unconditionally, so an unchanged badge forces a
    // dirty-rect + SPI flush every 2 s. -1 so the first tick always writes.
    int       cached_count;
};
static std::vector<Badge> s_badges;
static lv_timer_t* s_badge_timer = nullptr;

// IS.3: the home screen is built once and kept while apps run. It is rebuilt
// only when something the layout depends on changed.
struct LayoutKey {
    int style; bool dark; const lv_font_t* font; uint32_t visible_mask;
    int32_t w, h;
    bool operator==(const LayoutKey& o) const {
        return style == o.style && dark == o.dark && font == o.font &&
               visible_mask == o.visible_mask && w == o.w && h == o.h;
    }
};
static bool       s_built = false;
static LayoutKey  s_layout_key{};
static lv_obj_t*  s_last_focus = nullptr;
static LayoutKey current_layout_key(lv_obj_t* parent) {
    LayoutKey k{};
    k.style = (int)ui_style_get();
    k.dark = ui_dark_mode_get();
    k.font = get_home_font();
    for (int oi = 0; oi < kItemCount && oi < 32; oi++)
        if (home_apps_is_visible(oi)) k.visible_mask |= (1u << oi);
    lv_obj_update_layout(parent);
    k.w = lv_obj_get_width(parent);
    k.h = lv_obj_get_height(parent);
    return k;
}

// Tiles whose backing app needs an active WiFi connection — greyed out and
// click-blocked when WiFi is off or not connected — or, for tiles that also
// work over the phone link (phone_ok), when the PagerLink link is down.
// Tracked separately from the full tile list so the live tick only touches
// the ones that change.
struct WifiGatedTile {
    lv_obj_t* tile;
    lv_obj_t* icon;
    bool phone_ok;
};
static std::vector<WifiGatedTile> s_wifi_gated;
static int s_wifi_gated_last_state = -1;  // -1 = not yet applied

static ui_loading_t s_sync_loader;
static lv_timer_t*  s_sync_timer = nullptr;
static uint32_t     s_sync_deadline_ms = 0;

static void sync_poll_tick(lv_timer_t *t) {
    (void)t;
    int status = hw_get_time_sync_status();
    if (status == 1) {
        lv_timer_del(s_sync_timer);
        s_sync_timer = nullptr;
        ui_loading_close(&s_sync_loader);
        ui_msg_pop_up("Time Sync", "Time synchronized successfully!");
    } else if (lv_tick_get() > s_sync_deadline_ms) {
        lv_timer_del(s_sync_timer);
        s_sync_timer = nullptr;
        ui_loading_close(&s_sync_loader);
        ui_msg_pop_up("Time Sync", "Time sync timed out.");
    }
}

static void format_badge_text(int n, char *buf, size_t cap) {
    if (n > 99) snprintf(buf, cap, "99+");
    else        snprintf(buf, cap, "%d", n);
}

static void update_badges(lv_timer_t *t) {
    (void)t;
    INPUT_PROF_SPAN("badges");
    for (Badge &b : s_badges) {
        lv_obj_t *lbl = b.label;
        if (!lbl) continue;
        int n = b.fn ? b.fn() : 0;
        // P4.13a: skip the label write when the count hasn't changed.
        if (n == b.cached_count) continue;
        b.cached_count = n;
        if (n > 0) {
            char buf[8];
            format_badge_text(n, buf, sizeof(buf));
            lv_label_set_text(lbl, buf);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Pick a scalable icon font that comfortably fits the tile height while still
// leaving room for a label below it. Thresholds tuned for the three supported
// panels (240px, 192px, 380px-tall menu areas). Only uses sizes enabled in
// every build environment (including the native emulator).
const lv_font_t* pick_icon_font(int32_t tile_h) {
    if (tile_h >= 140) return &font_montserrat_48_subset;
    if (tile_h >= 90)  return &lv_font_montserrat_32;
    if (tile_h >= 70)  return &lv_font_montserrat_28;
    if (tile_h >= 55)  return &lv_font_montserrat_24;
    return &lv_font_montserrat_20;
}

// Resting background for tiles and pills — a hair above pure black so the
// accent border stays visible on OLED/AMOLED panels without bleeding into the
// page background.
static inline lv_color_t rest_bg() { return ui_color_surface(); }

// ---- Quick toggles (WiFi / BT) -------------------------------------------

struct QuickToggle {
    const char* icon;
    bool (*getter)();
    void (*setter)(bool);
};

static const QuickToggle kQuickToggles[] = {
    { LV_SYMBOL_WIFI,      hw_get_wifi_enable, hw_set_wifi_enable },
    { LV_SYMBOL_BLUETOOTH, hw_get_bt_enable,   hw_set_bt_enable   },
};
constexpr int kQuickToggleCount = sizeof(kQuickToggles) / sizeof(kQuickToggles[0]);

static void apply_toggle_style(lv_obj_t* btn, bool on) {
    if (on) {
        lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_ON_ACCENT, 0);
    } else {
        lv_obj_set_style_bg_color(btn, rest_bg(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_COLOR_MUTED, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_text_color(btn, UI_COLOR_MUTED, 0);
    }
    // Pixel Iso: the pill's lit top/left edge follows its state instead of the
    // muted resting border.
    if (ui_style_is_pixel()) {
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn,
            lv_color_lighten(on ? UI_COLOR_ACCENT : rest_bg(), LV_OPA_30), 0);
    }
    // Hotel / BeOS: the chrome keyline (Hotel's 2px black ring, BeOS's 1px
    // grey frame) plus a top-lit gradient, matching the theme's buttons.
    if (ui_style_has_chrome()) {
        const ui_palette_t &pal = ui_palette();
        lv_obj_set_style_bg_grad_color(btn, on ? lv_color_darken(UI_COLOR_ACCENT, LV_OPA_20)
                                               : lv_color_darken(rest_bg(), pal.grad_darken), 0);
        lv_obj_set_style_border_color(btn, ui_color_outline(), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, pal.keyline_w, 0);
    }
}

static void quick_toggle_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const QuickToggle* spec = (const QuickToggle*)lv_event_get_user_data(e);
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    if (!spec || !btn) return;
    bool new_state = !spec->getter();
    spec->setter(new_state);
    apply_toggle_style(btn, new_state);
    hw_feedback();
}

static void settings_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hw_feedback();
    core::System::getInstance().queueEnterApp("Settings");
}

// ---- Internet check ('p' home shortcut) ----------------------------------
// Quick TCP probe to 1.1.1.1:53 (mirrors Settings → Connectivity → Test
// Internet). The probe blocks for up to 3 s, so it runs on a OneShotJob
// behind a modal spinner; its delivery shows the rtt/error popup when it
// completes. The home screen stays live throughout instead of freezing.

// Render the probe outcome. Shared by the job's delivery and the synchronous
// task-spawn-failure fallback so the popup looks identical either way.
static void show_inet_result(bool ok, uint32_t rtt_ms, const std::string &err) {
    if (ok) {
        char rtt_buf[16];
        snprintf(rtt_buf, sizeof(rtt_buf), "%u ms", (unsigned)rtt_ms);
        ui_summary_row_t rows[] = {
            { "Host", "1.1.1.1:53" },
            { "RTT",  rtt_buf      },
        };
        ui_result_show("Internet " LV_SYMBOL_OK, "Online",
                       rows, sizeof(rows) / sizeof(rows[0]));
    } else {
        ui_result_show("Internet " LV_SYMBOL_CLOSE,
                       err.empty() ? "Failed" : err.c_str(),
                       nullptr, 0);
    }
}

struct InetCheckResult {
    bool        ok = false;
    uint32_t    rtt_ms = 0;
    std::string err;
};

static ui_loading_t s_inet_loading;

// Delivery (LVGL thread): close the spinner and show the popup.
static void inet_check_done(InetCheckResult &r) {
    ui_loading_close(&s_inet_loading);
    show_inet_result(r.ok, r.rtt_ms, r.err);
}

// The 3 s TCP probe runs off the LVGL thread on this job.
static core::OneShotJob<InetCheckResult> s_inet_job(inet_check_done);

// Tear down the in-flight check's UI (job delivery + spinner). A worker still
// running is detached; its result is freed on arrival. Called by
// MenuApp::onStop().
static void inet_check_teardown() {
    s_inet_job.teardown();
    ui_loading_close(&s_inet_loading);
}

static void run_internet_check() {
    if (!hw_get_wifi_connected()) {
        ui_result_show("Internet", "Wi-Fi is not connected.", nullptr, 0);
        return;
    }
    if (s_inet_job.busy()) return;   // one check at a time

    ui_loading_open(&s_inet_loading, "Internet", "Checking 1.1.1.1...");
    auto started = s_inet_job.start([]() {
        InetCheckResult *r = new InetCheckResult();
        r->ok = hw_ping_internet(&r->rtt_ms, &r->err);
        return r;
    }, 4096, "inet_chk", 1);
    if (started == core::OneShotJob<InetCheckResult>::Start::Failed) {
        // Task slots exhausted — fall back to a synchronous probe so the check
        // still works (this path pays the old up-to-3 s UI freeze).
        ui_loading_close(&s_inet_loading);
        uint32_t rtt_ms = 0;
        std::string err;
        bool ok = hw_ping_internet(&rtt_ms, &err);
        show_inet_result(ok, rtt_ms, err);
    }
}

// ---- Media controls (BLE HID transport keys to a paired phone) -----------
// Shown only when Bluetooth is enabled AND a BLE HID host has paired with us.
// Two buttons: play/pause (momentary) and volume. The volume button mirrors
// the Remote app — click to capture the encoder, rotate to send vol±, click
// again (or focus away) to release.
static std::vector<lv_obj_t*> s_media_buttons;  // tracked for visibility only
static lv_obj_t* s_volume_btn = nullptr;
static lv_obj_t* s_volume_icon = nullptr;
static lv_obj_t* s_wifi_btn = nullptr;  // WiFi quick-toggle pill, for 'w' shortcut
static lv_obj_t* s_bt_btn = nullptr;    // BT quick-toggle pill, for 'b' shortcut
static lv_timer_t* s_media_visibility_timer = nullptr;
static bool s_media_last_visible = false;

static bool media_controls_should_show() {
    return hw_get_bt_enable() && hw_get_ble_kb_connected();
}

static bool volume_is_editing() {
    if (!s_volume_btn) return false;
    lv_group_t* g = lv_obj_get_group(s_volume_btn);
    return g && lv_group_get_editing(g)
        && lv_group_get_focused(g) == s_volume_btn;
}

static void refresh_volume_visual(void) {
    if (!s_volume_btn || !s_volume_icon) return;
    bool active = volume_is_editing();
    if (active) {
        lv_obj_set_style_bg_color(s_volume_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_color(s_volume_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(s_volume_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_volume_icon, UI_COLOR_FG, 0);
    } else {
        lv_obj_set_style_bg_color(s_volume_btn, rest_bg(), 0);
        lv_obj_set_style_border_color(s_volume_btn, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(s_volume_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_volume_icon, UI_COLOR_ACCENT, 0);
    }
}

static void set_media_buttons_visible(bool show) {
    // The buttons live in the group from creation time (added before toggles
    // and tiles, so they sit at the front for encoder nav). Visibility is
    // purely a HIDDEN-flag toggle — lv_group_focus_next/prev already skips
    // hidden members, so we don't need to splice the group on every BT state
    // change. The old rebuild-the-group approach left the encoder indev
    // pointing at a stale focus slot when the timer fired post-boot, which
    // made the freshly-added pills unreachable until the user re-entered the
    // menu.
    lv_group_t* grp = lv_group_get_default();

    for (lv_obj_t* btn : s_media_buttons) {
        if (!btn) continue;
        if (show) {
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            // If the volume pill is mid-edit when we hide it, the encoder
            // would stay captured after it vanishes — drop editing first.
            if (btn == s_volume_btn && grp && lv_group_get_editing(grp)
                && lv_group_get_focused(grp) == s_volume_btn) {
                lv_group_set_editing(grp, false);
            }
            lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    refresh_volume_visual();
    s_media_last_visible = show;
}

static void apply_wifi_gated_state(bool wifi, bool link) {
    lv_group_t* grp = lv_group_get_default();
    bool needs_focus_advance = false;

    for (const WifiGatedTile& w : s_wifi_gated) {
        if (!w.tile || !w.icon) continue;
        const bool enabled = wifi || (w.phone_ok && link);
        if (enabled) {
            lv_obj_remove_state(w.tile, LV_STATE_DISABLED);
            lv_obj_set_style_border_color(w.tile, UI_COLOR_MUTED, 0);
            lv_obj_set_style_border_opa(w.tile, LV_OPA_30, 0);
            lv_obj_set_style_text_color(w.icon, UI_COLOR_ACCENT, 0);
            lv_obj_set_style_opa(w.tile, LV_OPA_COVER, 0);
        } else {
            // LV_STATE_DISABLED makes lv_group_focus_next/prev skip the tile,
            // so the encoder/keyboard nav jumps straight over it.
            lv_obj_add_state(w.tile, LV_STATE_DISABLED);
            lv_obj_set_style_border_color(w.tile, UI_COLOR_MUTED, 0);
            lv_obj_set_style_border_opa(w.tile, LV_OPA_30, 0);
            lv_obj_set_style_text_color(w.icon, UI_COLOR_MUTED, 0);
            // Dim the whole tile (including label) so the disabled state reads
            // at a glance even when not focused.
            lv_obj_set_style_opa(w.tile, LV_OPA_60, 0);
            // If this tile is currently focused (user toggled WiFi off while
            // sitting on it), nudge focus forward — otherwise the encoder
            // would still register a click on a now-inert tile.
            if (grp && lv_group_get_focused(grp) == w.tile) {
                needs_focus_advance = true;
            }
        }
    }

    if (needs_focus_advance && grp) lv_group_focus_next(grp);
}

// Re-apply the gated-tile visual when Wi-Fi / phone-link state changed since
// the last call (always after s_wifi_gated_last_state was reset to -1).
static void refresh_wifi_gated_state() {
    int state = (hw_get_wifi_connected() ? 1 : 0) | (hw_link_connected() ? 2 : 0);
    if (state == s_wifi_gated_last_state) return;
    apply_wifi_gated_state((state & 1) != 0, (state & 2) != 0);
    s_wifi_gated_last_state = state;
}

static void media_visibility_tick(lv_timer_t* t) {
    (void)t;
    INPUT_PROF_SPAN("media_vis");
    bool now = media_controls_should_show();
    if (now != s_media_last_visible) {
        set_media_buttons_visible(now);
    }
    // Re-evaluate WiFi-gated tiles on the same tick — cheap, and the user
    // can flip the WiFi pill or drop the phone link without leaving the menu.
    refresh_wifi_gated_state();
}

static void play_pause_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!hw_get_ble_kb_connected()) return;
    hw_feedback();
    hw_set_ble_key(MEDIA_PLAY_PAUSE);
}

// Home-screen single-letter shortcuts. Attached to every focusable object on
// the home screen below; LV_EVENT_KEY only fires on the focused widget, so
// this is dormant elsewhere in the app.
//   d → low-power loop (real ESP lightSleep / sleep, per sleep_mode)
//   w → toggle WiFi
//   b → toggle Bluetooth
//   p → run internet ping test
//   n → open Notes (Editor)
//   t → open Tasks
//   r → open Recorder
//   s → open Settings
//   z → NTP time sync (WiFi must be connected)
static void launch_app_from_home(const char* appName) {
    hw_feedback();
    core::System::getInstance().queueEnterApp(appName);
}

static void home_shortcut_key_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    uint32_t key = lv_event_get_key(e);
    if (key == 'd' || key == 'D') {
        hw_feedback();
        hw_low_power_loop();
    } else if (key == 'w' || key == 'W') {
        bool new_state = !hw_get_wifi_enable();
        hw_set_wifi_enable(new_state);
        if (s_wifi_btn) apply_toggle_style(s_wifi_btn, new_state);
        hw_feedback();
    } else if (key == 'b' || key == 'B') {
        bool new_state = !hw_get_bt_enable();
        hw_set_bt_enable(new_state);
        if (s_bt_btn) apply_toggle_style(s_bt_btn, new_state);
        hw_feedback();
    } else if (key == 'p' || key == 'P') {
        hw_feedback();
        run_internet_check();
    } else if (key == 'n' || key == 'N') {
        launch_app_from_home("Editor");
    } else if (key == 't' || key == 'T') {
        launch_app_from_home("Tasks");
    } else if (key == 'r' || key == 'R') {
        launch_app_from_home("Recorder");
    } else if (key == 's' || key == 'S') {
        launch_app_from_home("Settings");
    } else if (key == 'z' || key == 'Z') {
        hw_feedback();
        if (hw_get_wifi_connected()) {
            if (s_sync_timer) {
                return; // Already syncing
            }
            if (hw_start_time_sync_ntp()) {
                ui_loading_open(&s_sync_loader, "Time Sync", "Syncing from internet...");
                s_sync_deadline_ms = lv_tick_get() + 20000; // 20s deadline
                s_sync_timer = lv_timer_create(sync_poll_tick, 500, nullptr);
            } else {
                ui_msg_pop_up("Time Sync", "Failed to start sync");
            }
        } else {
            ui_msg_pop_up("Time Sync", "WiFi not connected");
        }
    }
}

void tile_click_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const HomeItem* item = (const HomeItem*)lv_event_get_user_data(e);
    if (!item || !item->appName) return;
    // WiFi- or phone-link-gated tiles (e.g. Telegram) are inert when neither
    // Wi-Fi is connected nor, for phone_ok tiles, the phone link is up —
    // click haptic only, no app switch. The greyed-out visual already
    // signals the disabled state, so a popup would just be noise.
    if (item->requires_wifi && !hw_get_wifi_connected()
        && !(item->phone_ok && hw_link_connected())) {
        hw_feedback();
        return;
    }
    launch_app_from_home(item->appName);
}

} // namespace

MenuApp::MenuApp() : core::App("MainMenu") {}

// Stop the live-state timers (badges, media/WiFi gating). Widgets stay.
static void stop_live_timers() {
    if (s_badge_timer) { lv_timer_del(s_badge_timer); s_badge_timer = nullptr; }
    if (s_media_visibility_timer) {
        lv_timer_del(s_media_visibility_timer);
        s_media_visibility_timer = nullptr;
    }
}

// Forget every cached widget pointer (tiles are about to be destroyed).
static void forget_widgets() {
    s_badges.clear();
    s_media_buttons.clear();
    s_volume_btn = nullptr;
    s_volume_icon = nullptr;
    s_wifi_btn = nullptr;
    s_bt_btn = nullptr;
    s_media_last_visible = false;
    s_wifi_gated.clear();
    s_wifi_gated_last_state = -1;
}

// Stop + forget. onStop only stops timers (the tiles persist, IS.3); a rebuild
// in onStart stops the timers and forgets the pointers, because lv_obj_clean
// just destroyed every widget they referenced.
static void reset_state() {
    stop_live_timers();
    forget_widgets();
}

// (Re)start the home screen's live state after a full build or a resume:
// badges, the WiFi-gated disabled visual, and the media/WiFi poll tick.
static void start_live_state() {
    // Populate badges immediately, then refresh on a slow tick so the menu
    // reflects unread counts that arrive while it's open.
    if (!s_badges.empty()) {
        update_badges(nullptr);
        s_badge_timer = lv_timer_create(update_badges, 2000, nullptr);
    }

    // Apply the disabled visual to wifi-gated tiles up front so the very
    // first frame already reflects the connection state — otherwise the
    // tile flashes its accent color until the 1 s tick fires.
    if (!s_wifi_gated.empty()) {
        s_wifi_gated_last_state = -1;
        refresh_wifi_gated_state();
    }

    // Poll BLE pairing state so media buttons appear/disappear while the
    // menu is open (user connects from their phone without leaving home),
    // and re-evaluate WiFi-gated tiles on the same tick.
    if (!s_media_buttons.empty() || !s_wifi_gated.empty()) {
        s_media_visibility_timer =
            lv_timer_create(media_visibility_tick, 1000, nullptr);
    }
}

// Returning home without a rebuild: refresh what may have changed while an
// app was open, restart the live timers and put focus back where it was.
static void resume() {
    if (s_wifi_btn) apply_toggle_style(s_wifi_btn, hw_get_wifi_enable());
    if (s_bt_btn)   apply_toggle_style(s_bt_btn, hw_get_bt_enable());
    set_media_buttons_visible(media_controls_should_show());
    for (auto& b : s_badges) b.cached_count = -1;
    s_wifi_gated_last_state = -1;
    start_live_state();
    lv_group_t* g = core::System::getInstance().getMenuGroup();
    if (!g) return;
    lv_obj_t* target = nullptr;
    if (s_last_focus && lv_obj_is_valid(s_last_focus) && lv_obj_get_group(s_last_focus) == g &&
        !lv_obj_has_flag(s_last_focus, LV_OBJ_FLAG_HIDDEN)) {
        target = s_last_focus;
    }
    if (target) lv_group_focus_obj(target);
    s_last_focus = nullptr;
}

void MenuApp::onStop() {
    inet_check_teardown();   // cancel any in-flight internet-check UI (worker self-finishes)
    // IS.3: keep the tiles; remember focus and pause the live state.
    lv_group_t* g = core::System::getInstance().getMenuGroup();
    s_last_focus = g ? lv_group_get_focused(g) : nullptr;
    stop_live_timers();
}

void MenuApp::onStart(lv_obj_t* parent) {
    INPUT_PROF_SPAN("MenuApp::onStart");
    LayoutKey key = current_layout_key(parent);
    if (s_built && key == s_layout_key && lv_obj_get_child_count(parent) > 0) {
        resume();
        return;
    }
    reset_state();          // timers + cached pointers die before the tiles do
    lv_obj_clean(parent);   // persistentUi(): AppManager no longer cleans for us
    s_built = true;
    s_layout_key = key;
    s_last_focus = nullptr;

    // Parent column: toggle strip on top, tile grid filling the rest.
    ui_apply_backdrop(parent);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 6, 0);
    lv_obj_set_style_pad_column(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_group_t* grp = core::System::getInstance().getMenuGroup();

    // Measure available room before carving out the toggle strip. The grid
    // gets whatever remains.
    lv_obj_update_layout(parent);
    int32_t panel_w = lv_obj_get_content_width(parent);
    int32_t panel_full_h = lv_obj_get_content_height(parent);
    if (panel_w <= 0) panel_w = 460;
    if (panel_full_h <= 0) panel_full_h = 180;

    // --- Quick-toggles strip --------------------------------------------------
    // Taller and more breathable than the first iteration so fingers can hit
    // the pills on the touch screens and the icons sit with enough margin to
    // not feel crowded against the tile grid below.
    const int32_t toggle_h = 30;
    lv_obj_t* toggle_bar = lv_obj_create(parent);
    lv_obj_set_size(toggle_bar, LV_PCT(100), toggle_h);
    lv_obj_set_style_bg_opa(toggle_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(toggle_bar, 0, 0);
    lv_obj_set_style_pad_all(toggle_bar, 0, 0);
    // Add horizontal padding so the 4px outline (2px pad + 2px width) on the outer pills
    // does not get clipped by the right/left edges of the parent container or screen.
    lv_obj_set_style_pad_right(toggle_bar, 6, 0);
    lv_obj_set_style_pad_left(toggle_bar, 6, 0);
    lv_obj_set_style_pad_column(toggle_bar, 8, 0);
    lv_obj_remove_flag(toggle_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toggle_bar, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(toggle_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toggle_bar, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto make_pill = [&](lv_obj_t* parent_bar,
                         const char* symbol,
                         int32_t w_extra) -> lv_obj_t* {
        lv_obj_t* btn = lv_btn_create(parent_bar);
        lv_obj_set_size(btn, toggle_h + w_extra, toggle_h);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(btn, toggle_h / 2, 0);
        lv_obj_set_style_bg_color(btn, rest_bg(), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        // Focus halo — white so it stays readable against an enabled
        // (accent-filled/bordered) pill, where an accent halo would blend in.
        lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(btn, UI_COLOR_FG, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_color(btn, UI_COLOR_FG, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_opa(btn, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUS_KEY);

        lv_obj_t* icon = lv_label_create(btn);
        lv_label_set_text(icon, symbol);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
        lv_obj_center(icon);
        lv_obj_set_user_data(btn, icon);
        ui_block(btn);
        return btn;
    };

    // --- Media pills (play/pause + volume) ---
    // Sit first in the strip, created hidden; a 1 s timer shows/hides
    // them based on BLE HID pairing state.
    lv_obj_t* play_btn = make_pill(toggle_bar, LV_SYMBOL_PLAY, 14);
    lv_obj_set_style_border_color(play_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(play_btn, LV_OPA_60, 0);
    lv_obj_t* play_icon = (lv_obj_t*)lv_obj_get_user_data(play_btn);
    lv_obj_set_style_text_color(play_icon, UI_COLOR_ACCENT, 0);
    lv_obj_add_event_cb(play_btn, play_pause_click_cb, LV_EVENT_CLICKED, nullptr);
    // Add to the group here so lv_group_focus_next/prev can skip them via the
    // HIDDEN flag while BLE is disconnected. Registering before the wifi/bt
    // toggles and app tiles keeps nav order matching the physical left-to-
    // right layout.
    if (grp) lv_group_add_obj(grp, play_btn);
    s_media_buttons.push_back(play_btn);

    s_volume_btn = make_pill(toggle_bar, LV_SYMBOL_VOLUME_MAX, 14);
    lv_obj_set_style_border_color(s_volume_btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(s_volume_btn, LV_OPA_60, 0);
    s_volume_icon = (lv_obj_t*)lv_obj_get_user_data(s_volume_btn);
    lv_obj_set_style_text_color(s_volume_icon, UI_COLOR_ACCENT, 0);
    // Same capture-the-wheel volume control as the Remote app; inert until
    // a phone is connected.
    ble_volume_btn_attach(s_volume_btn, refresh_volume_visual, /*require_connected=*/true);
    if (grp) lv_group_add_obj(grp, s_volume_btn);
    s_media_buttons.push_back(s_volume_btn);

    set_media_buttons_visible(media_controls_should_show());

    // --- WiFi / BT toggles ---
    for (int i = 0; i < kQuickToggleCount; i++) {
        const QuickToggle& qt = kQuickToggles[i];
        lv_obj_t* btn = make_pill(toggle_bar, qt.icon, 18);
        apply_toggle_style(btn, qt.getter());
        lv_obj_add_event_cb(btn, quick_toggle_click_cb, LV_EVENT_CLICKED,
                            (void*)&qt);
        if (grp) lv_group_add_obj(grp, btn);
        if (qt.getter == hw_get_wifi_enable) s_wifi_btn = btn;
        if (qt.getter == hw_get_bt_enable) s_bt_btn = btn;
    }

    // --- Settings shortcut ---
    // Sits at the rightmost of the strip — distinct from the on/off toggles:
    // always rendered in the muted resting style, click launches the Settings
    // app directly rather than mutating a flag.
    {
        lv_obj_t* settings_btn = make_pill(toggle_bar, LV_SYMBOL_SETTINGS, 18);
        apply_toggle_style(settings_btn, false);
        lv_obj_add_event_cb(settings_btn, settings_click_cb,
                            LV_EVENT_CLICKED, nullptr);
        if (grp) lv_group_add_obj(grp, settings_btn);
    }

    // --- Tile grid container (fills the rest) -------------------------------
    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Pixel Iso / Hotel / BeOS: the extrusion spills past the tile bounds into
    // the panel padding, so let it draw outside the grid container.
    if (ui_style_is_pixel() || ui_style_has_chrome()) lv_obj_add_flag(grid, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_update_layout(parent);
    int32_t grid_h = lv_obj_get_content_height(grid);
    if (grid_h <= 0) grid_h = panel_full_h - toggle_h - 6;
    if (grid_h < 50) grid_h = 50;

    // Count what's actually going on the grid first: layout has to scale to
    // the visible-tile count, not the total registry size. Without this the
    // row arithmetic below would still divide the available height by the
    // full kItemCount, leaving big empty bands when the user hides tiles.
    int visible_count = 0;
    for (int oi = 0; oi < kItemCount; oi++) {
        if (home_apps_is_visible(oi)) visible_count++;
    }

    // Empty state: user hid every tile. Drop a small hint so home isn't a
    // black void, and bail before the layout math (which would divide by 0).
    if (visible_count == 0) {
        lv_obj_t* hint = lv_label_create(grid);
        lv_label_set_text(hint, "No apps on home.\nSettings " LV_SYMBOL_RIGHT
                                " Home Apps to enable.");
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        // Drawn straight on the backdrop: muted grey, or white on the BeOS
        // desktop blue where the grey is illegible.
        lv_obj_set_style_text_color(hint, ui_palette().on_backdrop, 0);
        lv_obj_center(hint);
        // Still hand focus somewhere reachable so the encoder isn't stranded.
        if (grp && lv_obj_get_child_count(toggle_bar) > 0) {
            lv_group_focus_obj(lv_obj_get_child(toggle_bar, 0));
        }
        return;
    }

    // Pick a column count that pairs the tiles neatly:
    //   1 item    → 1 col   (full-width hero)
    //   2 items   → 2 cols
    //   4 items   → 2 cols  (2x2 reads better than 3+orphan)
    //   3 / 5+    → 3 cols  (default, matches the original layout)
    int cols;
    if (visible_count <= 1)      cols = 1;
    else if (visible_count == 2) cols = 2;
    else if (visible_count == 4) cols = 2;
    else                         cols = 3;
    const int gap = 8;
    // On short panels (the pager's grid is ~140 px) three rows can't reach
    // the 50 px minimum tile height and the centered grid would clip the top
    // and bottom rows. Trade height for width: add columns while the tiles
    // stay wide enough for their labels.
    const int32_t kMinTileH = 50;
    const int32_t kMinTileW = 64;
    while (cols < visible_count) {
        int r = (visible_count + cols - 1) / cols;
        if ((grid_h - (r - 1) * gap) / r >= kMinTileH) break;
        int nc = cols + 1;
        if ((panel_w - (nc - 1) * gap) / nc < kMinTileW) break;
        cols = nc;
    }
    int rows = (visible_count + cols - 1) / cols;
    int32_t tile_w = (panel_w - (cols - 1) * gap) / cols;
    int32_t tile_h = (grid_h - (rows - 1) * gap) / rows;
    if (tile_w < 50) tile_w = 50;
    if (tile_h < kMinTileH) {
        // Still too many rows for the panel (small watch screens): keep the
        // minimum height and let the grid scroll from the top instead of
        // clipping the first row behind the toggle strip.
        tile_h = kMinTileH;
        lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    }
    // Cap tile size so a single-tile (or 2-up) layout doesn't blow into a
    // giant button. The original 3x3 sizing implicitly capped these — once
    // the grid scales down it's worth keeping the upper bound.
    const int32_t kMaxTileW = 180;
    const int32_t kMaxTileH = 140;
    if (tile_w > kMaxTileW) tile_w = kMaxTileW;
    if (tile_h > kMaxTileH) tile_h = kMaxTileH;

    const lv_font_t* icon_font = pick_icon_font(tile_h);
    const lv_font_t* label_font = get_home_font();

    for (int oi = 0; oi < kItemCount; oi++) {
        // Honor user's home-screen visibility preference. Hidden tiles are
        // skipped entirely — they don't take a grid slot and aren't in the
        // focus group, but the underlying app remains registered and can
        // still be launched from Settings/shortcuts.
        if (!home_apps_is_visible(oi)) continue;
        const HomeItem& item = kItems[oi];
        lv_color_t accent = UI_COLOR_ACCENT;

        lv_obj_t* tile = lv_btn_create(grid);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        // Resting look: near-black fill with a faint muted border. All tiles
        // share the same neutral chrome so the grid reads as one surface.
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_set_style_bg_color(tile, rest_bg(), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, UI_COLOR_MUTED, 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_30, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_outline_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 6, 0);

        // Focused: accent border at full strength and an outer accent halo
        // so the selected tile pops without changing fill color.
        lv_obj_set_style_border_color(tile, accent, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(tile, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(tile, accent, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(tile, 3, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_opa(tile, LV_OPA_40, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_pad(tile, 2, LV_STATE_FOCUSED);

        // Pressed: brief flash via a slightly lighter neutral fill.
        lv_obj_set_style_bg_color(tile, lv_color_mix(UI_COLOR_FG, rest_bg(), LV_OPA_10),
                                  LV_STATE_PRESSED);
        ui_block(tile);

        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, 3, 0);

        lv_obj_t* icon = lv_label_create(tile);
        lv_label_set_text(icon, item.symbol);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, accent, 0);
        // On focus, swap to FG so the icon pops white against the accent
        // border/halo — without this the focused tile has no icon-color cue.
        lv_obj_set_style_text_color(icon, UI_COLOR_FG, LV_STATE_FOCUSED);

        lv_obj_t* label = lv_label_create(tile);
        lv_label_set_text(label, item.label);
        lv_obj_set_style_text_color(label, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(label, label_font, 0);
        lv_obj_set_width(label, tile_w - 10);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        // DOT only truncates with a fixed height; auto height wraps instead.
        lv_obj_set_height(label, lv_font_get_line_height(label_font));

        lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED,
                            (void*)&item);
        if (grp) lv_group_add_obj(grp, tile);

        if (item.requires_wifi) {
            s_wifi_gated.push_back({tile, icon, item.phone_ok});
        }

        // Tiles with a badge_fn get a small red pill in the top-right
        // corner, hidden by default and toggled on by update_badges().
        if (item.badge_fn) {
            lv_obj_t* badge = lv_label_create(tile);
            lv_label_set_text(badge, "");
            lv_obj_add_flag(badge, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -2, 2);
            lv_obj_set_style_bg_color(badge, lv_palette_main(LV_PALETTE_RED), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(badge, UI_COLOR_ON_ACCENT, 0);
            lv_obj_set_style_text_font(badge, &lv_font_montserrat_14, 0);
            lv_obj_set_style_radius(badge, ui_palette().pill_radius, 0);
            lv_obj_set_style_pad_hor(badge, 5, 0);
            lv_obj_set_style_pad_ver(badge, 1, 0);
            lv_obj_add_flag(badge, LV_OBJ_FLAG_HIDDEN);
            s_badges.push_back({badge, item.badge_fn, -1}); // -1 = never written
        }
    }

    // Default focus to the first tile rather than a quick-toggle — tiles are
    // the primary nav target; encoder-back lands on the toggles.
    if (grp && lv_obj_get_child_count(grid) > 0) {
        lv_group_focus_obj(lv_obj_get_child(grid, 0));
    }

    start_live_state();

    // Wire the home-screen letter shortcuts ('d'/'w'/'b'/'p'/...) on every
    // focusable object: LV_EVENT_KEY fires on the focused widget only, so
    // attaching to each group member makes the shortcut work regardless of
    // which tile/pill the user happens to be sitting on.
    if (grp) {
        uint32_t n = lv_group_get_obj_count(grp);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t* o = lv_group_get_obj_by_index(grp, i);
            if (o) lv_obj_add_event_cb(o, home_shortcut_key_cb,
                                       LV_EVENT_KEY, nullptr);
        }
    }
}

// ---- Home apps visibility API -------------------------------------------
// NVS-backed (Preferences "homeapps" namespace), keyed by appName so the
// stored value survives reordering kItems. Default is "shown" — a missing
// slot returns true. The settings UI flips slots; the menu reads them on
// each rebuild (see the loop in onStart).

namespace {

// NVS keys are capped at 15 chars including the null. "v_" prefix keeps us
// well clear of the limit (longest current appName is "Telegram" → 10).
constexpr const char *kHomeAppsNs = "homeapps";

void make_vis_key(const char *appName, char out[16]) {
    snprintf(out, 16, "v_%s", appName ? appName : "");
}

// One-shot RAM cache of the per-tile visibility flags. home_apps_is_visible()
// is called ~2x per tile on every menu rebuild; without this each call ran its
// own NVS begin/getBool/end (up to ~28 opens per rebuild). Filled by a single
// NVS pass on first use and kept in sync by home_apps_set_visible().
int8_t s_vis_cache[kItemCount];
bool   s_vis_loaded = false;

void load_vis_cache() {
#ifdef ARDUINO
    Preferences p;
    bool ok = p.begin(kHomeAppsNs, true);
    for (int i = 0; i < kItemCount; i++) {
        if (!ok) { s_vis_cache[i] = 1; continue; }
        char key[16];
        make_vis_key(kItems[i].appName, key);
        s_vis_cache[i] = p.getBool(key, true) ? 1 : 0;
    }
    if (ok) p.end();
#else
    // Emulator: each tile's flag is a file under $PAGERLINK_NVS_DIR/homeapps,
    // so the grid layout survives a restart. A missing slot still means shown.
    for (int i = 0; i < kItemCount; i++) {
        char key[16];
        make_vis_key(kItems[i].appName, key);
        s_vis_cache[i] = hal::nvs_get_bool(kHomeAppsNs, key, true) ? 1 : 0;
    }
#endif
    s_vis_loaded = true;
}

} // anonymous namespace

int home_apps_count() { return kItemCount; }

const char *home_apps_label(int idx) {
    if (idx < 0 || idx >= kItemCount) return "";
    return kItems[idx].label;
}

const char *home_apps_name(int idx) {
    if (idx < 0 || idx >= kItemCount) return "";
    return kItems[idx].appName;
}

bool home_apps_is_visible(int idx) {
    if (idx < 0 || idx >= kItemCount) return false;
    if (!s_vis_loaded) load_vis_cache();
    return s_vis_cache[idx] != 0;
}

void home_apps_set_visible(int idx, bool on) {
    if (idx < 0 || idx >= kItemCount) return;
    if (!s_vis_loaded) load_vis_cache();
    s_vis_cache[idx] = on ? 1 : 0;
    char key[16];
    make_vis_key(kItems[idx].appName, key);
    hal::nvs_set_bool(kHomeAppsNs, key, on);
}

} // namespace apps
