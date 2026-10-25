/**
 * @file      ui_wifi.cpp
 * @brief     Modal WiFi network picker: scan, connect, forget saved creds.
 *
 * Opened from the Connectivity settings subpage. Renders on lv_layer_top so
 * it fully covers the settings menu (and the status-bar back button). Uses
 * the existing passphrase prompt for password entry.
 */
#include "ui_define.h"
#include "core/input_focus.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct WifiCtx {
    ui_overlay_t ov        = {};
    lv_obj_t *status_label = nullptr;
    lv_obj_t *list         = nullptr;
    lv_obj_t *scan_btn     = nullptr;
    lv_obj_t *forget_btn   = nullptr;
    lv_timer_t *poll_timer = nullptr;
    bool scan_in_flight    = false;
    bool connect_in_flight = false;
    std::string pending_ssid;
    std::string pending_password;   // cached for save-on-success
    uint32_t connect_deadline_ms = 0;
    // Snapshot of the networks currently rendered in the list, in display
    // order. Clicks look them up here instead of re-querying the scan driver
    // (an instance-mutex pull), so synthesised entries (e.g. the connected
    // network shown during a scan) still resolve correctly.
    struct Shown {
        std::string ssid;
        uint8_t authmode;
    };
    std::vector<Shown> displayed;
};

static WifiCtx *g_ctx = nullptr;

static void set_status(const char *msg)
{
    if (!g_ctx || !g_ctx->status_label) return;
    lv_label_set_text(g_ctx->status_label, msg ? msg : "");
}

static void populate_list(const std::vector<wifi_scan_params_t> &nets);

// Re-render the list from the driver's latest scan; returns the network count.
static size_t refresh_scan_results()
{
    std::vector<wifi_scan_params_t> nets;
    hw_get_wifi_scan_result(nets);
    populate_list(nets);
    return nets.size();
}

// Populate the list with just the currently-connected network (if any).
// Lets users see their network immediately while the scan is still in flight;
// the scan-complete handler will replace this with the full list.
static void populate_connected_only()
{
    std::vector<wifi_scan_params_t> placeholder;
    if (hw_get_wifi_connected()) {
        wifi_scan_params_t p;
        std::string ssid;
        hw_get_wifi_ssid(ssid);
        p.ssid = ssid;
        p.rssi = hw_get_wifi_rssi();
        // We don't have the real authmode until the scan completes, but
        // since we're associated it's almost certainly secured — mark it as
        // non-OPEN so the list doesn't render a misleading "open" hint.
        p.authmode = 1;
        p.channel = 0;
        placeholder.push_back(p);
    }
    populate_list(placeholder);
}

static void start_scan()
{
    if (!g_ctx || g_ctx->scan_in_flight) return;
    if (!hw_get_wifi_enable()) {
        hw_set_wifi_enable(true);
    }
    set_status("Scanning...");
    populate_connected_only();
    hw_set_wifi_scan();
    g_ctx->scan_in_flight = true;
}

static void tear_down();

static void close_event_cb(lv_event_t *e)
{
    (void)e;
    tear_down();
}

static void scan_event_cb(lv_event_t *e)
{
    (void)e;
    start_scan();
}

static void forget_event_cb(lv_event_t *e)
{
    (void)e;
    hw_wifi_forget();
    set_status("Saved network removed.");
    if (g_ctx && g_ctx->forget_btn) {
        lv_obj_add_state(g_ctx->forget_btn, LV_STATE_DISABLED);
    }
}

static void kick_off_connect(const std::string &ssid, const std::string &pw)
{
    if (!g_ctx) return;
    wifi_conn_params_t p;
    p.ssid     = ssid;
    p.password = pw;
    hw_set_wifi_connect(p);
    g_ctx->pending_ssid = ssid;
    g_ctx->pending_password = pw;
    g_ctx->connect_in_flight = true;
    g_ctx->connect_deadline_ms = lv_tick_get() + 20000; /* 20 s timeout */
    char buf[96];
    snprintf(buf, sizeof(buf), "Connecting to %s...", ssid.c_str());
    set_status(buf);
}

/* Password prompt OK path: fire connect and start poll for association. */
static void password_entered_cb(const char *pw, void *ud)
{
    (void)ud;
    if (!g_ctx) return;
    if (!pw) {
        /* User cancelled. */
        set_status("Cancelled.");
        return;
    }
    kick_off_connect(g_ctx->pending_ssid, pw);
}

static void net_clicked_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    if (!g_ctx || !btn) return;
    intptr_t idx = (intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || (size_t)idx >= g_ctx->displayed.size()) return;
    /* Entries synthesised before the scan completes (the connected-network
     * placeholder) carry authmode 1 (unknown/locked), so the connect path
     * still works. */
    std::string ssid = g_ctx->displayed[idx].ssid;
    uint8_t authmode = g_ctx->displayed[idx].authmode;
    g_ctx->pending_ssid = ssid;

    /* Tapping the currently-connected network disconnects instead of
     * re-associating. Gives users a way to drop a connection without
     * forgetting (which would wipe the saved password). */
    if (hw_get_wifi_connected()) {
        std::string cur_ssid;
        hw_get_wifi_ssid(cur_ssid);
        if (cur_ssid == ssid) {
            hw_set_wifi_disconnect();
            g_ctx->connect_in_flight = false;
            char buf[96];
            snprintf(buf, sizeof(buf), "Disconnected from %s.", ssid.c_str());
            set_status(buf);
            refresh_scan_results();
            return;
        }
    }

    /* Already a favorite — reuse the stored password so the user doesn't
     * have to re-enter it. Works for open networks too (saved password is
     * empty). */
    std::string saved_pw;
    if (hw_wifi_get_saved_password(ssid, saved_pw)) {
        kick_off_connect(ssid, saved_pw);
        return;
    }

    /* Open network — authmode 0 is WIFI_AUTH_OPEN on ESP32. */
    if (authmode == 0) {
        kick_off_connect(ssid, "");
        return;
    }

    char title[96];
    snprintf(title, sizeof(title), "Connect to %s", ssid.c_str());
    ui_passphrase_prompt(title, "Enter WiFi password", /*confirm=*/false,
                         password_entered_cb, nullptr);
}

static const char *authmode_label(uint8_t mode)
{
    /* ESP-IDF wifi_auth_mode_t mapping. Anything that's not OPEN gets a
     * lock badge; the specifics (WPA2/WPA3/etc.) aren't worth exposing. */
    return (mode == 0) ? "" : LV_SYMBOL_WARNING " ";
}

static bool is_saved_ssid(const std::string &ssid,
                          const std::vector<std::string> &saved)
{
    for (const auto &s : saved) {
        if (s == ssid) return true;
    }
    return false;
}

static void populate_list(const std::vector<wifi_scan_params_t> &nets)
{
    if (!g_ctx || !g_ctx->list) return;
    lv_obj_clean(g_ctx->list);
    g_ctx->displayed.clear();

    if (nets.empty()) {
        ui_list_add_empty_label(g_ctx->list, "No networks found");
        return;
    }

    // Favorites first, preserving the scan's RSSI ordering within each group.
    std::vector<std::string> saved;
    hw_wifi_get_saved_list(saved);
    std::vector<size_t> order;
    order.reserve(nets.size());
    for (size_t i = 0; i < nets.size(); ++i) {
        if (is_saved_ssid(nets[i].ssid, saved)) order.push_back(i);
    }
    for (size_t i = 0; i < nets.size(); ++i) {
        if (!is_saved_ssid(nets[i].ssid, saved)) order.push_back(i);
    }

    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        const wifi_scan_params_t &n = nets[i];
        bool fav = is_saved_ssid(n.ssid, saved);
        char label[160];
        snprintf(label, sizeof(label), "%s%s%s  (%d dBm)",
                 fav ? LV_SYMBOL_OK " " : "",
                 authmode_label(n.authmode), n.ssid.c_str(), (int)n.rssi);
        lv_obj_t *btn = lv_list_add_btn(g_ctx->list, LV_SYMBOL_WIFI, label);
        // user_data holds the display index into g_ctx->displayed.
        lv_obj_set_user_data(btn, (void *)(intptr_t)g_ctx->displayed.size());
        g_ctx->displayed.push_back({n.ssid, n.authmode});
        lv_obj_add_event_cb(btn, net_clicked_cb, LV_EVENT_CLICKED, nullptr);
        lv_group_add_obj(g_ctx->ov.group, btn);
    }
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_ctx) return;
    // Pause while the password prompt is open (its textarea is focused) —
    // hw_get_wifi_scan_result does a scan-result pull over the instance
    // mutex that the keyboard reader also needs.
    if (ui_text_input_focused()) return;

    if (g_ctx->scan_in_flight && !hw_get_wifi_scanning()) {
        g_ctx->scan_in_flight = false;
        size_t found = refresh_scan_results();
        char buf[64];
        snprintf(buf, sizeof(buf), "Found %u network(s).", (unsigned)found);
        set_status(buf);
    }

    if (g_ctx->connect_in_flight) {
        if (hw_get_wifi_connected()) {
            g_ctx->connect_in_flight = false;
            // Save only after association succeeds so bad passwords don't
            // stick in the favorites list.
            hw_wifi_add_saved(g_ctx->pending_ssid, g_ctx->pending_password);
            std::string ip;
            hw_get_ip_address(ip);
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected. IP: %s", ip.c_str());
            set_status(buf);
            if (g_ctx->forget_btn) {
                lv_obj_remove_state(g_ctx->forget_btn, LV_STATE_DISABLED);
            }
            // Refresh the list so the new favorite gets its star badge.
            refresh_scan_results();
        } else if ((int32_t)(lv_tick_get() - g_ctx->connect_deadline_ms) > 0) {
            g_ctx->connect_in_flight = false;
            set_status("Connection failed or timed out.");
        }
    }
}

static void tear_down()
{
    if (!g_ctx) return;
    WifiCtx *ctx = g_ctx;
    g_ctx = nullptr;

    if (ctx->poll_timer) {
        lv_timer_del(ctx->poll_timer);
    }
    // Also restores the previous back-button handler so the underlying
    // screen (the Settings subpage that opened us) keeps working.
    ui_overlay_close(&ctx->ov);
    delete ctx;
}

} /* namespace */

void ui_wifi_networks_open()
{
    if (g_ctx) return;
    WifiCtx *ctx = new WifiCtx();
    g_ctx = ctx;

    // Re-point the status-bar back button at our close handler so both touch
    // and keyboard navigation land on a working "back" inside this overlay.
    // The overlay sits just below the status bar instead of covering it —
    // users still see time/battery/etc. while picking a network.
    lv_obj_t *overlay = ui_overlay_open(&ctx->ov, close_event_cb, false);

    /* Button row: Scan, Forget. Close is handled by the status-bar back
     * button, re-pointed to close_event_cb above. */
    lv_obj_t *row = lv_obj_create(overlay);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    ctx->scan_btn   = ui_modal_btn_create(row, LV_SYMBOL_REFRESH " Scan",  scan_event_cb, nullptr);
    ctx->forget_btn = ui_modal_btn_create(row, LV_SYMBOL_TRASH   " Forget", forget_event_cb, nullptr);
    lv_obj_set_flex_grow(ctx->scan_btn,   1);
    lv_obj_set_flex_grow(ctx->forget_btn, 1);
    if (!hw_wifi_has_saved()) {
        lv_obj_add_state(ctx->forget_btn, LV_STATE_DISABLED);
    }
    lv_group_add_obj(ctx->ov.group, ctx->scan_btn);
    lv_group_add_obj(ctx->ov.group, ctx->forget_btn);

    /* Status line under the buttons. */
    ctx->status_label = lv_label_create(overlay);
    lv_label_set_text(ctx->status_label, "");
    lv_obj_set_style_text_color(ctx->status_label, UI_COLOR_MUTED, 0);
    lv_label_set_long_mode(ctx->status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->status_label, lv_pct(100));
    lv_obj_set_style_text_align(ctx->status_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Scrollable list of networks grows to fill the remaining space. */
    ctx->list = lv_list_create(overlay);
    lv_obj_set_width(ctx->list, lv_pct(100));
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_set_style_radius(ctx->list, UI_RADIUS, 0);

    lv_group_focus_obj(ctx->scan_btn);

    ctx->poll_timer = lv_timer_create(poll_cb, 500, nullptr);

    /* If the radio's already associated (e.g. auto-reconnect succeeded on
     * boot), surface that up front. */
    if (hw_get_wifi_connected()) {
        std::string ssid, ip;
        hw_get_wifi_ssid(ssid);
        hw_get_ip_address(ip);
        char buf[160];
        snprintf(buf, sizeof(buf), "Connected: %s (%s)", ssid.c_str(), ip.c_str());
        set_status(buf);
    }

    /* Kick off an initial scan so the list isn't empty on open. */
    start_scan();
}
