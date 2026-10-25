/**
 * @file      system.cpp
 * @author    Gemini CLI
 * @license   MIT
 * @copyright Copyright (c) 2026
 */
#include "system.h"
#include "input_focus.h"
#include "input_profile.h"
#include "notify.h"
#include "ui_post.h"
#include "../ui_define.h"
#include "../hal/hub.h"

#include "../apps/menu_app.h"

#include <atomic>
#include <vector>

LV_FONT_DECLARE(lv_font_montserrat_18);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_32);


namespace core {

System& System::getInstance() {
    static System instance;
    return instance;
}

void System::init() {
    setupGlobalUI();

    // Render banners for notify::post() calls from any task. Installed here
    // so the renderer exists before the first app can fire a notification.
    notify::install_default_renderer();

    // Register the MainMenu app
    AppManager::getInstance().registerApp(std::make_shared<apps::MenuApp>());

    // Start with the menu
    AppManager::getInstance().switchApp("MainMenu", _menuPanel);
    showMenu();
}

namespace {
// Last colour written to each status-bar icon (lv_color_to_u32), so the 1 s
// refresh only calls lv_obj_set_style_text_color on a change (P6.7).
// applyChromeStyle repaints the icons directly, so it resets these.
uint32_t prev_sd_color   = UINT32_MAX;
uint32_t prev_bt_color   = UINT32_MAX;
uint32_t prev_wifi_color = UINT32_MAX;
uint32_t prev_hub_color  = UINT32_MAX;
uint32_t prev_usb_color  = UINT32_MAX;

void reset_icon_color_cache()
{
    prev_sd_color = prev_bt_color = prev_wifi_color =
        prev_hub_color = prev_usb_color = UINT32_MAX;
}
} // namespace

void System::setupGlobalUI() {
    theme_init();

    _menuGroup = lv_group_create();
    lv_group_set_wrap(_menuGroup, false);

    // IS.4: apps get their own group so the (persistent, IS.3) home widgets
    // never sit in an app's focus order, and an app starts with an empty
    // group whose editing flag is cleared.
    _appGroup = lv_group_create();
    lv_group_set_wrap(_appGroup, false);

    // Set default group to menu group initially
    lv_group_set_default(_menuGroup);

    int32_t v_res = lv_display_get_vertical_resolution(NULL);
    if (v_res <= 0) v_res = 222;

    const lv_font_t *header_font = get_header_font();
    int32_t bar_h = ui_status_bar_height();

    // Create Main Screen (TileView)
    _mainScreen = lv_tileview_create(lv_screen_active());
    lv_obj_set_size(_mainScreen, LV_PCT(100), v_res - bar_h);
    lv_obj_align(_mainScreen, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(_mainScreen, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(_mainScreen, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(_mainScreen, LV_SCROLLBAR_MODE_OFF);

    _menuPanel = lv_tileview_add_tile(_mainScreen, 0, 0, LV_DIR_NONE);
    _appPanel = lv_tileview_add_tile(_mainScreen, 0, 1, LV_DIR_NONE);

    // Create Status Bar
    _statusBar = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_statusBar, LV_PCT(100), bar_h);
    lv_obj_align(_statusBar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_statusBar, ui_color_titlebar(), 0);
    lv_obj_set_style_bg_opa(_statusBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_statusBar, 0, 0);
    lv_obj_set_style_radius(_statusBar, 0, 0);
    lv_obj_set_style_pad_all(_statusBar, 2, 0);

    _statTimeLabel = lv_label_create(_statusBar);
    lv_obj_center(_statTimeLabel);
    lv_obj_set_style_text_color(_statTimeLabel, ui_color_title_fg(), 0);
    lv_obj_set_style_text_font(_statTimeLabel, header_font, 0);
    /* Avoid LVGL's default "Text" leaking through any modal (e.g. the
     * passphrase prompt) that opens before the 1Hz timer below has fired. */
    lv_label_set_text(_statTimeLabel, "");

    // Right-side flex container holding SD, USB and battery indicators.
    // Flex order: SD, USB, Battery — so hidden icons collapse naturally and
    // the battery stays pinned to the far right with SD/USB to its left.
    _statRightCont = lv_obj_create(_statusBar);
    lv_obj_set_size(_statRightCont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(_statRightCont, LV_ALIGN_RIGHT_MID, -5, 0);
    lv_obj_set_flex_flow(_statRightCont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_statRightCont, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(_statRightCont, 0, 0);
    lv_obj_set_style_pad_column(_statRightCont, 4, 0);
    lv_obj_set_style_border_width(_statRightCont, 0, 0);
    lv_obj_set_style_bg_opa(_statRightCont, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(_statRightCont, LV_OBJ_FLAG_SCROLLABLE);

    // File-count indicator — added first so it sits at the leftmost position
    // inside the right-side cluster; styled gray per spec.
    _statFileCountLabel = lv_label_create(_statRightCont);
    lv_obj_set_style_text_color(_statFileCountLabel, ui_color_title_muted(), 0);
    lv_obj_set_style_text_font(_statFileCountLabel, header_font, 0);
    lv_label_set_text(_statFileCountLabel, "");
    lv_obj_add_flag(_statFileCountLabel, LV_OBJ_FLAG_HIDDEN);

    _statSDLabel = lv_label_create(_statRightCont);
    lv_obj_set_style_text_color(_statSDLabel, ui_color_title_icon(), 0);
    lv_obj_set_style_text_font(_statSDLabel, header_font, 0);
    lv_label_set_text(_statSDLabel, LV_SYMBOL_SD_CARD);
    lv_obj_add_flag(_statSDLabel, LV_OBJ_FLAG_HIDDEN);

    _statUSBLabel = lv_label_create(_statRightCont);
    lv_obj_set_style_text_color(_statUSBLabel, ui_color_title_icon(), 0);
    lv_obj_set_style_text_font(_statUSBLabel, header_font, 0);
    lv_label_set_text(_statUSBLabel, LV_SYMBOL_USB);
    lv_obj_add_flag(_statUSBLabel, LV_OBJ_FLAG_HIDDEN);

    _statBTLabel = lv_label_create(_statRightCont);
    lv_obj_set_style_text_color(_statBTLabel, ui_color_title_icon(), 0);
    lv_obj_set_style_text_font(_statBTLabel, header_font, 0);
    lv_label_set_text(_statBTLabel, LV_SYMBOL_BLUETOOTH);
    lv_obj_add_flag(_statBTLabel, LV_OBJ_FLAG_HIDDEN);

    _statWifiLabel = lv_label_create(_statRightCont);
    lv_obj_set_style_text_color(_statWifiLabel, ui_color_title_icon(), 0);
    lv_obj_set_style_text_font(_statWifiLabel, header_font, 0);
    lv_label_set_text(_statWifiLabel, LV_SYMBOL_WIFI);
    lv_obj_add_flag(_statWifiLabel, LV_OBJ_FLAG_HIDDEN);

    // Local hub indicator. Hidden when the user hasn't enabled the hub at
    // all; muted when enabled but unreachable; accent when the LAN service
    // answered our last TCP probe. The probe runs from the status timer
    // below at a much slower cadence than the 1Hz refresh.
    _statHubLabel = lv_label_create(_statRightCont);
    lv_obj_set_style_text_color(_statHubLabel, ui_color_title_muted(), 0);
    lv_obj_set_style_text_font(_statHubLabel, header_font, 0);
    lv_label_set_text(_statHubLabel, LV_SYMBOL_HOME);
    lv_obj_add_flag(_statHubLabel, LV_OBJ_FLAG_HIDDEN);

    _statBattLabel = lv_label_create(_statRightCont);
    lv_obj_set_style_text_color(_statBattLabel, ui_color_title_fg(), 0);
    lv_obj_set_style_text_font(_statBattLabel, header_font, 0);
    lv_label_set_text(_statBattLabel, "");

    _statMemLabel = lv_label_create(_statusBar);
    lv_obj_align(_statMemLabel, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_set_style_text_color(_statMemLabel, ui_color_title_fg(), 0);
    lv_obj_set_style_text_font(_statMemLabel, header_font, 0);
    lv_obj_add_flag(_statMemLabel, LV_OBJ_FLAG_HIDDEN);

    // Back button — hidden until a screen requests it. Sits on the far left
    // of the status bar; left-aligned icons (SD/USB/MEM) shift right when it's
    // visible so they don't overlap.
    _statBackBtn = lv_btn_create(_statusBar);
    lv_obj_set_size(_statBackBtn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(_statBackBtn, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_radius(_statBackBtn, 0, 0);
    lv_obj_set_style_pad_hor(_statBackBtn, 4, 0);
    lv_obj_set_style_pad_ver(_statBackBtn, 0, 0);
    lv_obj_set_style_border_width(_statBackBtn, 0, 0);
    lv_obj_set_style_outline_width(_statBackBtn, 0, 0);
    lv_obj_set_style_shadow_width(_statBackBtn, 0, 0);
    lv_obj_set_style_bg_opa(_statBackBtn, LV_OPA_TRANSP, 0);
    lv_obj_t *back_lbl = lv_label_create(_statBackBtn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, header_font, 0);
    lv_obj_set_style_text_color(back_lbl, ui_color_title_fg(), 0);
    lv_obj_add_flag(_statBackBtn, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create([](lv_timer_t *t) {
        System& self = System::getInstance();

        // While the user types, skip only the two costly reads: the gauge I2C
        // sweep and the FFat file-count walk. The clock, radio icons and the
        // USB "unsafe to disconnect" box keep updating — the keyboard reader no
        // longer needs the instance mutex (IS.1), so the old blanket hold only
        // froze them (P7.17).
        const bool typing = core::isTextInputFocused();

        // P4.10: per-tick caches for labels whose text rarely changes.
        // LVGL 9 lv_label_set_text* unconditionally copies the string and calls
        // lv_obj_invalidate(), so every unchanged write forces a dirty-rect +
        // SPI flush — defeating PB.8's 200 ms idle cap on a static screen.
        // Initialised to sentinel values that guarantee a write on the first tick.
        static int      prev_clock_min    = -1, prev_clock_hour = -1;
        static int      prev_clock_mday   = -1, prev_clock_mon  = -1;
        static int      prev_clock_year   = -1;
        static int      prev_batt_pct     = -1;
        static bool     prev_is_charging  = false;
        static uint32_t prev_file_count   = UINT32_MAX; // UINT32_MAX = never written
        static uint32_t prev_mem_free_kb  = UINT32_MAX;

        static const lv_font_t *applied_header_font = nullptr;
        const lv_font_t *cur_font = get_header_font();
        if (applied_header_font == nullptr) applied_header_font = cur_font;
        if (self._statusBar && cur_font != applied_header_font) {
            applied_header_font = cur_font;
            int32_t new_bar_h = ui_status_bar_height();  // from cur_font
            lv_obj_set_style_text_font(self._statTimeLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statBattLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statMemLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statSDLabel, cur_font, 0);
            lv_obj_set_style_text_font(self._statUSBLabel, cur_font, 0);
            if (self._statBTLabel) {
                lv_obj_set_style_text_font(self._statBTLabel, cur_font, 0);
            }
            if (self._statWifiLabel) {
                lv_obj_set_style_text_font(self._statWifiLabel, cur_font, 0);
            }
            if (self._statHubLabel) {
                lv_obj_set_style_text_font(self._statHubLabel, cur_font, 0);
            }
            if (self._statFileCountLabel) {
                lv_obj_set_style_text_font(self._statFileCountLabel, cur_font, 0);
            }
            if (self._statBackBtn) {
                lv_obj_t *back_lbl = lv_obj_get_child(self._statBackBtn, 0);
                if (back_lbl) lv_obj_set_style_text_font(back_lbl, cur_font, 0);
            }
            if (lv_obj_get_height(self._statusBar) != new_bar_h) {
                lv_obj_set_height(self._statusBar, new_bar_h);
                int32_t v_res = lv_display_get_vertical_resolution(NULL);
                if (v_res <= 0) v_res = 222;
                lv_obj_set_height(self._mainScreen, v_res - new_bar_h);
            }
            // Font changed: invalidate caches so the next tick re-writes label
            // text with the new font metrics.
            prev_clock_min = prev_clock_hour = prev_clock_mday =
                prev_clock_mon = prev_clock_year = -1;
            prev_batt_pct    = -1;
            prev_file_count  = UINT32_MAX;
            prev_mem_free_kb = UINT32_MAX;
            reset_icon_color_cache();
        }

        // Status bar update logic. Use the system clock (not the RTC over
        // I2C) for this once-per-second label — see hw_get_wall_clock.
        struct tm timeinfo;
        hw_get_wall_clock(timeinfo);
        if (self._statTimeLabel &&
            (timeinfo.tm_min  != prev_clock_min  ||
             timeinfo.tm_hour != prev_clock_hour ||
             timeinfo.tm_mday != prev_clock_mday ||
             timeinfo.tm_mon  != prev_clock_mon  ||
             timeinfo.tm_year != prev_clock_year)) {
            lv_label_set_text_fmt(self._statTimeLabel, "%02d/%02d/%04d %02d:%02d",
                                timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
                                timeinfo.tm_hour, timeinfo.tm_min);
            prev_clock_min  = timeinfo.tm_min;
            prev_clock_hour = timeinfo.tm_hour;
            prev_clock_mday = timeinfo.tm_mday;
            prev_clock_mon  = timeinfo.tm_mon;
            prev_clock_year = timeinfo.tm_year;
        }

        if (!typing) {
            monitor_params_t params;
            hw_get_monitor_params(params);
            if (self._statBattLabel &&
                (params.battery_percent != prev_batt_pct ||
                 params.is_charging     != prev_is_charging)) {
                lv_label_set_text_fmt(self._statBattLabel, "%s %d%%",
                                      ui_battery_icon(params.battery_percent, params.is_charging),
                                      params.battery_percent);
                prev_batt_pct    = params.battery_percent;
                prev_is_charging = params.is_charging;
            }
        }

        // When the back button is visible, push the memory readout right so
        // it doesn't overlap.
        int back_off = 0;
        if (self._statBackBtn && !lv_obj_has_flag(self._statBackBtn, LV_OBJ_FLAG_HIDDEN)) {
            back_off = lv_obj_get_width(self._statBackBtn);
            if (back_off <= 0) back_off = 20;
            back_off += 4;
        }

        // lv_obj_set_style_text_color invalidates even when the colour is the
        // same, which made these icons repaint every second (P6.7). Only write
        // on a change.
        auto set_icon_color = [](lv_obj_t *obj, lv_color_t c, uint32_t &prev) {
            uint32_t v = lv_color_to_u32(c);
            if (v == prev) return;
            prev = v;
            lv_obj_set_style_text_color(obj, c, 0);
        };

        bool sd_online = (HW_SD_ONLINE & hw_get_device_online());
        if (self._statSDLabel) {
            if (sd_online) {
                lv_obj_add_flag(self._statSDLabel, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(self._statSDLabel, LV_OBJ_FLAG_HIDDEN);
                set_icon_color(self._statSDLabel, lv_palette_main(LV_PALETTE_RED), prev_sd_color);
            }
        }

        // Radio icons have three states each:
        //   off        → hidden
        //   on, idle   → shown in muted gray
        //   connected  → shown in accent color
        if (self._statBTLabel) {
            if (hw_get_bt_enable()) {
                lv_obj_clear_flag(self._statBTLabel, LV_OBJ_FLAG_HIDDEN);
                set_icon_color(self._statBTLabel,
                    hw_get_ble_kb_connected() ? ui_color_title_icon() : ui_color_title_muted(), prev_bt_color);
            } else {
                lv_obj_add_flag(self._statBTLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (self._statWifiLabel) {
            if (hw_get_wifi_enable()) {
                lv_obj_clear_flag(self._statWifiLabel, LV_OBJ_FLAG_HIDDEN);
                set_icon_color(self._statWifiLabel,
                    hw_get_wifi_connected() ? ui_color_title_icon() : ui_color_title_muted(), prev_wifi_color);
            } else {
                lv_obj_add_flag(self._statWifiLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Hub indicator. The TCP probe is a real network call (a connect()
        // with a 1.5s timeout). We offload it to a background FreeRTOS task
        // so it doesn't stutter the LVGL timer thread.
        if (self._statHubLabel) {
            if (hal::hub_is_enabled()) {
                static std::atomic<bool> hub_reachable{false};
                static std::atomic<bool> hub_probe_running{false};
                static uint8_t hub_tick = 0;

                // Probe every 30 s, matching the freshness window the consumers
                // actually demand (hub_last_reachable's 30 s max_age). Probing
                // faster bought nothing but an extra task spawn + TCP SYN (radio
                // wake) per tick; the status-bar icon now lags <=30 s. (PB.10)
                if (!hub_probe_running && ++hub_tick >= 30) {
                    hub_tick = 0;
                    // No Wi-Fi: the probe can only fail, so don't spawn a task for it (P7.40).
                    if (!hw_get_wifi_connected()) {
                        hub_reachable = false;
                    } else {
                        hub_probe_running = true;
#ifdef ARDUINO
                        void **args = (void**)malloc(2 * sizeof(void*));
                        if (args == nullptr) {
                            hub_probe_running = false;
                        } else {
                            args[0] = (void*)&hub_reachable;
                            args[1] = (void*)&hub_probe_running;
                            BaseType_t spawned = xTaskCreate([](void *arg) {
                                bool res = hal::hub_is_reachable();
                                hal::hub_note_reachable(res);   // publish for non-blocking readers
                                auto *reachable_ptr = (std::atomic<bool>*)((void**)arg)[0];
                                auto *running_ptr = (std::atomic<bool>*)((void**)arg)[1];
                                reachable_ptr->store(res);
                                running_ptr->store(false);
                                free(arg);
                                vTaskDelete(NULL);
                            }, "hub_probe", 3072, args, 1, NULL);
                            if (spawned != pdPASS) {
                                free(args);
                                hub_probe_running = false;
                            }
                        }
#else
                        hub_reachable = hal::hub_is_reachable();
                        hal::hub_note_reachable(hub_reachable.load());
                        hub_probe_running = false;
#endif
                    }
                }
                lv_obj_clear_flag(self._statHubLabel, LV_OBJ_FLAG_HIDDEN);
                set_icon_color(self._statHubLabel,
                    hub_reachable.load() ? ui_color_title_icon() : ui_color_title_muted(), prev_hub_color);
            } else {
                lv_obj_add_flag(self._statHubLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        bool usb_mounted = hw_is_usb_msc_mounted();

        if (self._statUSBLabel) {
            if (usb_mounted) {
                lv_obj_clear_flag(self._statUSBLabel, LV_OBJ_FLAG_HIDDEN);
                if (hw_is_usb_msc_writing()) {
                    set_icon_color(self._statUSBLabel, lv_palette_main(LV_PALETTE_RED), prev_usb_color);
                } else if (hw_is_usb_msc_reading()) {
                    set_icon_color(self._statUSBLabel, lv_palette_main(LV_PALETTE_BLUE), prev_usb_color);
                } else {
                    set_icon_color(self._statUSBLabel, ui_color_title_icon(), prev_usb_color);
                }
            } else {
                lv_obj_add_flag(self._statUSBLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // Unsafe-to-disconnect warning: present as long as the volume is
        // mounted on the host. Sits on the top layer so it overrides whatever
        // app is currently running, and tears itself down once the host
        // ejects the volume.
        if (usb_mounted && self._usbWarningBox == nullptr) {
            // Untitled ui_popup_create plus a red frame.
            lv_obj_t *box = ui_popup_create(NULL);
            lv_obj_set_style_border_color(box, lv_palette_main(LV_PALETTE_RED), 0);
            lv_obj_set_style_border_width(box, UI_BORDER_W, 0);
            lv_obj_set_style_radius(box, 0, 0);

            lv_obj_t *icon = lv_label_create(box);
            lv_label_set_text(icon, LV_SYMBOL_WARNING);
            lv_obj_set_style_text_color(icon, lv_palette_main(LV_PALETTE_RED), 0);
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);

            lv_obj_t *title = lv_label_create(box);
            lv_label_set_text(title, "Unsafe to\ndisconnect");
            lv_obj_set_style_text_color(title, UI_COLOR_FG, 0);
            lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
            lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

            lv_obj_t *msg = lv_label_create(box);
            lv_label_set_text(msg,
                "Eject the USB\n"
                "volume before\n"
                "unplugging.");
            lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(msg, lv_pct(95));
            lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(msg, UI_COLOR_FG, 0);
            lv_obj_set_style_text_font(msg, &lv_font_montserrat_18, 0);

            self._usbWarningBox = box;
        } else if (!usb_mounted && self._usbWarningBox != nullptr) {
            lv_obj_del(self._usbWarningBox);
            self._usbWarningBox = nullptr;
        }

        user_setting_params_t settings;
        hw_get_user_setting(settings);

        // Internal file count — refreshed only every few ticks so the FFat
        // walk stays cheap. Tick counter is local to this timer.
        if (self._statFileCountLabel) {
            if (settings.show_file_count) {
                static uint8_t file_count_tick = 0;
                static uint32_t cached_file_count = UINT32_MAX;
                if (!typing && (cached_file_count == UINT32_MAX || ++file_count_tick >= 60)) {
                    cached_file_count = hw_count_internal_files();
                    file_count_tick = 0;
                }
                if (cached_file_count != prev_file_count) {
                    lv_label_set_text_fmt(self._statFileCountLabel, "%u", (unsigned)cached_file_count);
                    prev_file_count = cached_file_count;
                }
                lv_obj_clear_flag(self._statFileCountLabel, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(self._statFileCountLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        if (self._statMemLabel) {
            if (settings.show_mem_usage) {
                uint32_t mem_free_kb = hw_get_free_heap() / 1024;
                if (mem_free_kb != prev_mem_free_kb) {
                    lv_label_set_text_fmt(self._statMemLabel, "M:%uK", mem_free_kb);
                    prev_mem_free_kb = mem_free_kb;
                }
                lv_obj_clear_flag(self._statMemLabel, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_x(self._statMemLabel, 5 + back_off);
            } else {
                lv_obj_add_flag(self._statMemLabel, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }, 1000, NULL);

    /* Populate clock and battery synchronously so they're visible before the
     * first 1Hz tick. If a passphrase modal (or any textarea) opens during the
     * first second of boot, the timer's text-input gate would otherwise freeze
     * these labels at their empty state — and the dimmed status bar would
     * appear blank behind the modal. */
    {
        struct tm timeinfo;
        hw_get_date_time(timeinfo);
        lv_label_set_text_fmt(_statTimeLabel, "%02d/%02d/%04d %02d:%02d",
                              timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
                              timeinfo.tm_hour, timeinfo.tm_min);

        monitor_params_t params;
        hw_get_monitor_params(params);
        lv_label_set_text_fmt(_statBattLabel, "%s %d%%",
                              ui_battery_icon(params.battery_percent, params.is_charging),
                              params.battery_percent);
    }

    applyChromeStyle();
}

void System::applyChromeStyle() {
    // Re-tint the persistent chrome to the current palette. Called after a
    // style switch and once at the end of setupGlobalUI.
    if (_mainScreen) {
        lv_obj_set_style_bg_color(_mainScreen, UI_COLOR_BG, 0);
    }
    // Apps paint their panel with a local UI_COLOR_BG (chat, ssh, weather,
    // ...) that outlives them; re-tint it so a page without its own fill
    // (Settings) doesn't show the previous palette.
    if (_appPanel)  lv_obj_set_style_bg_color(_appPanel, UI_COLOR_BG, 0);
    if (_menuPanel) lv_obj_set_style_bg_color(_menuPanel, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_color(lv_screen_active(), UI_COLOR_BG, 0);
    if (_statusBar) {
        lv_obj_set_style_bg_color(_statusBar, ui_color_titlebar(), 0);
        // A seam under the bar: Pixel Iso's accent rule, a darker blue under
        // the Habbo title bar, a thin gold one under the BeOS tab; Classic none.
        const ui_palette_t &pal = ui_palette();
        lv_obj_set_style_border_width(_statusBar, pal.seam_w, 0);
        if (pal.seam_w) {
            lv_obj_set_style_border_side(_statusBar, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(_statusBar, pal.title_seam, 0);
            lv_obj_set_style_border_opa(_statusBar, LV_OPA_COVER, 0);
        }
    }
    // Re-color the status-bar members to sit on the title bar.
    if (_statTimeLabel)     lv_obj_set_style_text_color(_statTimeLabel, ui_color_title_fg(), 0);
    if (_statBattLabel)     lv_obj_set_style_text_color(_statBattLabel, ui_color_title_fg(), 0);
    if (_statMemLabel)      lv_obj_set_style_text_color(_statMemLabel, ui_color_title_fg(), 0);
    if (_statBackBtn) {
        lv_obj_t *lbl = lv_obj_get_child(_statBackBtn, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, ui_color_title_fg(), 0);
    }
    if (_statSDLabel)       lv_obj_set_style_text_color(_statSDLabel, ui_color_title_icon(), 0);
    if (_statUSBLabel)      lv_obj_set_style_text_color(_statUSBLabel, ui_color_title_icon(), 0);
    if (_statBTLabel)       lv_obj_set_style_text_color(_statBTLabel, ui_color_title_icon(), 0);
    if (_statWifiLabel)     lv_obj_set_style_text_color(_statWifiLabel, ui_color_title_icon(), 0);
    if (_statFileCountLabel) lv_obj_set_style_text_color(_statFileCountLabel, ui_color_title_muted(), 0);
    reset_icon_color_cache();   // the next status tick re-applies the state colours
}

void System::showMenu() {
    AppManager::getInstance().switchApp("MainMenu", _menuPanel);
    lv_group_set_editing(_menuGroup, false);
    // set_default_group() links every input device to the menu group.
    set_default_group(_menuGroup);

    lv_tileview_set_tile_by_index(_mainScreen, 0, 0, LV_ANIM_OFF);
    enable_keyboard(); // Ensure keyboard is enabled in menu
    // switchApp()'s dump ran before the menu group became the default again.
    INPUT_PROF(prof::dump_group("home"));
}

void System::hideMenu() {
    lv_tileview_set_tile_by_index(_mainScreen, 0, 1, LV_ANIM_OFF);
}

void System::enterApp(const char* name) {
    if (!name) return;
    auto& am = AppManager::getInstance();
    if (am.isActive(name)) { hideMenu(); return; }
    hideMenu();
    lv_group_remove_all_objs(_appGroup);
    lv_group_set_editing(_appGroup, false);
    set_default_group(_appGroup);
    am.switchApp(name, _appPanel);
}

void System::queueShowMenu() {
    _pendingShowMenu = true;
    _pendingEnterApp = nullptr;
}

void System::queueEnterApp(const char* name) {
    _pendingEnterApp = name;
    _pendingShowMenu = false;
}

bool System::transitionPending() const {
    return _pendingShowMenu || _pendingEnterApp != nullptr ||
           AppManager::getInstance().hasPendingSwitch();
}

void System::loop() {
    AppManager::getInstance().update();
    // IS.5: transitions queued from event callbacks run here, after
    // lv_timer_handler() has finished dispatching, so an app is never torn
    // down from inside its own widget's event.
    if (const char* name = _pendingEnterApp) {
        _pendingEnterApp = nullptr;
        enterApp(name);
    }
    if (_pendingShowMenu) {
        _pendingShowMenu = false;
        showMenu();
    }
    // P7.47: closures posted from other tasks (core/ui_post.h).
    ui_post_drain();
    // Drain pending notifications on the LVGL thread. `post()` is safe to
    // call from any task; actual banner creation has to happen here so it
    // runs under the instance lock held by the LVGL task.
    notify::pump();
    INPUT_PROF(prof::report_tick());
}

// Drop every event callback currently registered on the status-bar back
// button. Passing NULL to lv_obj_remove_event_cb matches only callbacks whose
// cb field is NULL (i.e. nothing), so stale handlers from previously-opened
// apps would otherwise accumulate and fire alongside the current app's cb.
//
// Iterate by a fixed snapshot of the count: during event dispatch LVGL marks
// handlers for deletion but defers actual removal until traversal ends, so
// the event count doesn't decrease mid-dispatch. Re-querying in a loop would
// spin forever when this is called from inside a CLICKED handler (e.g. the
// back button's own callback tearing down the app).
static void clear_back_button_events(lv_obj_t *btn) {
    if (!btn) return;
    uint32_t n = lv_obj_get_event_count(btn);
    // Iterate high→low: outside a dispatch the array shifts as items are
    // removed, and descending indices keep the remaining positions valid.
    // Inside a dispatch lv_event only marks for deletion (the array is
    // stable until traversal ends), so either direction works there.
    for (uint32_t i = n; i > 0; i--) {
        lv_obj_remove_event(btn, i - 1);
    }
}

lv_obj_t* System::showBackButton(lv_event_cb_t cb) {
    if (!_statBackBtn) return nullptr;
    clear_back_button_events(_statBackBtn);
    // No wait-release hook any more: CLICKED fires on the key release, and
    // every leave-the-app path is deferred (queueShowMenu / queueEnterApp,
    // IS.5), so the next screen is built after the press has fully ended.
    // lv_indev_wait_release() here used to swallow the first key of a fast
    // burst on the new screen.
    if (cb) {
        lv_obj_add_event_cb(_statBackBtn, cb, LV_EVENT_CLICKED, NULL);
    }
    _backBtnCb = cb;
    lv_obj_remove_flag(_statBackBtn, LV_OBJ_FLAG_HIDDEN);
    lv_group_t *g = lv_group_get_default();
    if (g) {
        // lv_group_add_obj appends, so if the group already held other
        // widgets (e.g. when restoring this handler after a modal overlay
        // tore down) the back button would land at the end of the nav
        // order. Rebuild the group so the back button sits at index 0 while
        // every other member keeps its original relative order.
        lv_group_remove_obj(_statBackBtn);
        // IS.4: size the snapshot from the actual member count. The old fixed
        // 32-entry cap silently dropped members of large groups (e.g. a long
        // Settings subpage), losing them from the nav order.
        uint32_t n = lv_group_get_obj_count(g);
        std::vector<lv_obj_t*> members(n);
        for (uint32_t i = 0; i < n; i++) {
            members[i] = lv_group_get_obj_by_index(g, i);
        }
        lv_group_remove_all_objs(g);
        lv_group_add_obj(g, _statBackBtn);
        for (uint32_t i = 0; i < n; i++) {
            if (members[i]) lv_group_add_obj(g, members[i]);
        }
    }
    return _statBackBtn;
}

void System::hideBackButton() {
    if (!_statBackBtn) return;
    clear_back_button_events(_statBackBtn);
    _backBtnCb = nullptr;
    lv_group_remove_obj(_statBackBtn);
    lv_obj_add_flag(_statBackBtn, LV_OBJ_FLAG_HIDDEN);
}

} // namespace core

// Free-function wrappers declared in ui/back_button.h so ui_*.cpp files don't need
// to pull in the core::System singleton directly.
lv_obj_t *ui_show_back_button(lv_event_cb_t cb)
{
    return core::System::getInstance().showBackButton(cb);
}

void ui_hide_back_button(void)
{
    core::System::getInstance().hideBackButton();
}

lv_event_cb_t ui_get_back_button_cb(void)
{
    return core::System::getInstance().getBackButtonCb();
}
