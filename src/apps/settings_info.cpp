/**
 * @file      settings_info.cpp
 * @brief     Settings » System Info subpage. Extracted from ui_settings.cpp;
 *            see settings_internal.h for the cross-TU contract.
 *
 * Read-only info rows: MAC, WiFi SSID/IP/RSSI, battery, RAM + PSRAM heap,
 * SD + internal storage usage, LVGL version, Arduino core version, build
 * date, firmware hash, chip id. A 1 Hz lv_timer refreshes the live rows
 * (RSSI, battery voltage, heap — P7.49); the RTC clock row is updated every
 * 5 s to avoid per-second I2C traffic (P4.13b), starting on the first tick.
 * Storage + SSID + IP are loaded once on first tick (gated by `info_loaded`)
 * because they're expensive.
 */
#include "../ui_define.h"
#include "settings_internal.h"

namespace info_cfg {

namespace {

struct sys_label_t {
    lv_obj_t *datetime_label         = nullptr;
    lv_obj_t *wifi_rssi_label        = nullptr;
    lv_obj_t *batt_voltage_label     = nullptr;
    lv_obj_t *heap_int_label         = nullptr;
    lv_obj_t *heap_psram_label       = nullptr;

    lv_obj_t *wifi_ssid_label        = nullptr;
    lv_obj_t *ip_info_label          = nullptr;
    lv_obj_t *sd_size_label          = nullptr;
    lv_obj_t *storage_used_label     = nullptr;
    lv_obj_t *storage_free_label     = nullptr;
    lv_obj_t *local_storage_total_label = nullptr;
    lv_obj_t *local_storage_used_label  = nullptr;
    lv_obj_t *local_storage_free_label  = nullptr;
    bool      info_loaded            = false;
    // P4.13b: cadence counter + per-label caches to avoid redundant I2C reads
    // and unconditional lv_label_set_text* calls (which call lv_obj_invalidate
    // in LVGL 9 regardless of whether the text changed).
    // All reset to defaults by `sys_label = sys_label_t{}` in reset_state().
    // Increments each 1 s tick; the RTC is read every 5th. Starts at 4 so the
    // first tick already reads it instead of showing the placeholder for 5 s.
    uint8_t   rtc_tick              = 4;
    std::string cached_datetime;              // last string written to datetime_label
    int       cached_rssi           = 1;      // 1 = sentinel (valid RSSI is always <= 0)
    int       cached_batt_mv        = -1;     // -1 = sentinel (valid voltage > 0)
    // KB-granular signatures so the 1 Hz tick only rewrites on a change.
    uint32_t  cached_heap_key       = UINT32_MAX;
    uint32_t  cached_psram_key      = UINT32_MAX;
};

sys_label_t sys_label;
lv_timer_t *info_timer = nullptr;
// Captured in build_subpage so the 1 Hz refresh can skip its RTC/PMIC I2C reads
// while this page is off-screen — lv_menu keeps the built page (and this timer)
// alive after the user navigates away.
lv_obj_t *s_menu = nullptr;
lv_obj_t *s_page = nullptr;

void sys_timer_event_cb(lv_timer_t *)
{
    // Only do the live reads while this page is actually on screen; otherwise
    // the RTC + fuel gauge got polled at 1 Hz across the whole Settings app
    // after one visit here. Fully torn down on Settings exit (reset_state).
    if (!s_menu || lv_menu_get_cur_main_page(s_menu) != s_page) return;

    // P4.13b: read the PCF85063 over I2C only every 5 s (the timer fires at
    // 1 Hz so other rows keep their 1 s update cadence). The cached_datetime
    // compare also skips the label write when the second-display text hasn't
    // changed — lv_label_set_text_fmt in LVGL 9 calls lv_obj_invalidate()
    // unconditionally.
    if (++sys_label.rtc_tick >= 5) {
        sys_label.rtc_tick = 0;
        std::string datetime;
        hw_get_date_time(datetime);
        if (datetime != sys_label.cached_datetime) {
            sys_label.cached_datetime = datetime;
            lv_label_set_text_fmt(sys_label.datetime_label, "%s", datetime.c_str());
        }
    }

    if (hw_get_wifi_connected()) {
        int rssi = hw_get_wifi_rssi();
        if (rssi != sys_label.cached_rssi) {
            sys_label.cached_rssi = rssi;
            lv_label_set_text_fmt(sys_label.wifi_rssi_label, "%d", rssi);
        }
    }
    // Read through the TTL-cached sweep rather than hw_get_battery_voltage(),
    // which forces a full gauge.refresh() every second. <=5 s staleness on a
    // static voltage label is fine, and this shares the status bar's cache. (PB.15)
    monitor_params_t mp;
    hw_get_monitor_params(mp);
    if (mp.battery_voltage != sys_label.cached_batt_mv) {
        sys_label.cached_batt_mv = mp.battery_voltage;
        lv_label_set_text_fmt(sys_label.batt_voltage_label, "%d mV", mp.battery_voltage);
    }

    // Heap telemetry (P7.49). Keys are KB-granular mixes of the counters, so
    // the 1 Hz tick only rewrites a label when a value actually moved.
    heap_stats_t hs;
    hw_get_heap_stats(hs);
    uint32_t int_free_kb    = hs.int_free / 1024;
    uint32_t int_min_kb     = hs.int_min_free / 1024;
    uint32_t int_largest_kb = hs.int_largest / 1024;
    uint32_t heap_key = (int_free_kb << 20) ^ (int_min_kb << 10) ^ int_largest_kb;
    if (heap_key != sys_label.cached_heap_key) {
        sys_label.cached_heap_key = heap_key;
        lv_label_set_text_fmt(sys_label.heap_int_label, "%uK free, min %uK, blk %uK",
                              (unsigned)int_free_kb, (unsigned)int_min_kb,
                              (unsigned)int_largest_kb);
    }

    uint32_t psram_free_kb = hs.psram_free / 1024;
    uint32_t psram_min_kb  = hs.psram_min_free / 1024;
    uint32_t psram_key = (psram_free_kb << 16) ^ psram_min_kb;
    if (psram_key != sys_label.cached_psram_key) {
        sys_label.cached_psram_key = psram_key;
        lv_label_set_text_fmt(sys_label.heap_psram_label, "%uK free, min %uK",
                              (unsigned)psram_free_kb, (unsigned)psram_min_kb);
    }

    if (!sys_label.info_loaded) {
        std::string wifi_ssid = "N/A";
        hw_get_wifi_ssid(wifi_ssid);
        lv_label_set_text(sys_label.wifi_ssid_label, wifi_ssid.c_str());

        std::string ip_info = "N/A";
        hw_get_ip_address(ip_info);
        lv_label_set_text(sys_label.ip_info_label, ip_info.c_str());

        auto format_size = [](char *buf, size_t len, uint64_t bytes) {
            if (bytes > 1024ULL * 1024 * 1024) {
                snprintf(buf, len, "%.2f GB", bytes / 1024.0 / 1024.0 / 1024.0);
            } else if (bytes > 1024 * 1024) {
                snprintf(buf, len, "%.2f MB", bytes / 1024.0 / 1024.0);
            } else if (bytes > 1024) {
                snprintf(buf, len, "%.2f KB", bytes / 1024.0);
            } else {
                snprintf(buf, len, "%llu B", bytes);
            }
        };
        // Total / used / free rows of one volume; "N/A" when it reports 0.
        auto show_volume = [&](uint64_t total, uint64_t used, uint64_t free,
                               lv_obj_t *total_lbl, lv_obj_t *used_lbl, lv_obj_t *free_lbl) {
            const uint64_t vals[3] = {total, used, free};
            lv_obj_t *const lbls[3] = {total_lbl, used_lbl, free_lbl};
            for (int i = 0; i < 3; i++) {
                char buffer[64];
                if (total > 0) format_size(buffer, sizeof(buffer), vals[i]);
                else           snprintf(buffer, sizeof(buffer), "N/A");
                lv_label_set_text(lbls[i], buffer);
            }
        };

        uint64_t total = 0, used = 0, free = 0;
        hw_get_storage_info(total, used, free);
        show_volume(total, used, free, sys_label.sd_size_label,
                    sys_label.storage_used_label, sys_label.storage_free_label);

        uint64_t l_total = 0, l_used = 0, l_free = 0;
        hw_get_local_storage_info(l_total, l_used, l_free);
        show_volume(l_total, l_used, l_free, sys_label.local_storage_total_label,
                    sys_label.local_storage_used_label, sys_label.local_storage_free_label);

        sys_label.info_loaded = true;
    }
}

} // anonymous namespace

void reset_state()
{
    if (info_timer) {
        lv_timer_del(info_timer);
        info_timer = nullptr;
    }
    s_menu = nullptr;
    s_page = nullptr;
    sys_label = sys_label_t{};
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    s_menu = menu;
    s_page = sub_page;
    lv_obj_add_flag(sub_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sub_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(sub_page, 4, 0);
    lv_obj_set_style_pad_row(sub_page, 0, 0);

    auto add_info_row = [&](const char *key, const char *val) {
        return settings_kv_row(sub_page, key, val);
    };

    char buffer[128];
    uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    bool has_mac = hw_get_mac(mac);
    if (has_mac) {
        snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    add_info_row("MAC", has_mac ? buffer : "N/A");

    sys_label.wifi_ssid_label = add_info_row("WiFi", "Loading...");
    sys_label.datetime_label = add_info_row("RTC", "00:00:00");
    sys_label.ip_info_label = add_info_row("IP", "Loading...");
    sys_label.wifi_rssi_label = add_info_row("RSSI", "N/A");

    snprintf(buffer, sizeof(buffer), "%d mV", hw_get_battery_voltage());
    sys_label.batt_voltage_label = add_info_row("Battery", buffer);

    sys_label.heap_int_label = add_info_row("RAM", "...");
    sys_label.heap_psram_label = add_info_row("PSRAM", "...");

#if defined(HAS_SD_CARD_SOCKET)
    const char *storage_name = "SD Total";
#else
    const char *storage_name = "Storage Total";
#endif
    sys_label.sd_size_label = add_info_row(storage_name, "Loading...");
    sys_label.storage_used_label = add_info_row("Storage Used", "Loading...");
    sys_label.storage_free_label = add_info_row("Storage Free", "Loading...");

    sys_label.local_storage_total_label = add_info_row("Internal Total", "Loading...");
    sys_label.local_storage_used_label = add_info_row("Internal Used", "Loading...");
    sys_label.local_storage_free_label = add_info_row("Internal Free", "Loading...");

    snprintf(buffer, sizeof(buffer), "%d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());
    add_info_row("LVGL", buffer);

    std::string ver;
    hw_get_arduino_version(ver);
    add_info_row("Core", ver.c_str());

    add_info_row("Built", __DATE__);
    add_info_row("Hash", hw_get_firmware_hash_string());
    add_info_row("Chip", hw_get_chip_id_string());

    // Builders run once per Settings session and reset_state() kills the
    // timer on exit, so no prior timer exists here.
    info_timer = lv_timer_create(sys_timer_event_cb, 1000, NULL);
}

} // namespace info_cfg
