/**
 * @file      wireless.h
 * @brief     WiFi and BLE (including BLE keyboard).
 */
#pragma once

#include "types.h"

// --- WiFi ---
void hw_get_wifi_ssid(std::string &param);
void hw_get_ip_address(std::string &param);
int16_t hw_get_wifi_rssi();

int16_t hw_set_wifi_scan();
bool hw_get_wifi_scanning();
void hw_get_wifi_scan_result(std::vector<wifi_scan_params_t> &list);
void hw_set_wifi_connect(wifi_conn_params_t &params);
bool hw_get_wifi_connected();
void hw_set_wifi_disconnect();

bool hw_get_wifi_enable();
void hw_set_wifi_enable(bool en);

// --- WiFi saved credentials (persistent across reboots) ---
// Favorites list: every network the user has successfully connected to is
// stored; the most recent lands at index 0.
bool hw_wifi_has_saved();
bool hw_wifi_get_saved_password(const std::string &ssid, std::string &password);
void hw_wifi_get_saved_list(std::vector<std::string> &ssids);
void hw_wifi_add_saved(const std::string &ssid, const std::string &password);
void hw_wifi_forget();  // drop every saved network
bool hw_wifi_forget_ssid(const std::string &ssid);  // drop one saved network; true when saved

// P4.17: WiFi reconnect supervisor with exponential backoff.
// Call from the main loop at any cadence >= 1 Hz; the function self-throttles.
// Retries hw_wifi_reconnect_saved() at 30 s -> 60 s -> 5 min (cap); after
// 5 consecutive failures drops to WIFI_OFF and retries after 15 min.
// Resets all state on a successful association. setAutoReconnect stays false.
// Driven from loop() in factory.ino.
void hw_wifi_supervise(uint32_t now_ms);

// PB.2 — power-save escalation for fake sleep.
// hw_wifi_powersave_sleep() escalates to WIFI_PS_MAX_MODEM when connected,
// letting the RF front-end sleep for the listen interval during fake sleep.
// hw_wifi_powersave_active() restores WIFI_PS_MIN_MODEM on wake.
// Both are no-ops when WiFi is disconnected or (on the emulator) unconstrained.
void hw_wifi_powersave_sleep();
void hw_wifi_powersave_active();

// --- HTTP (blocking; require an active WiFi connection) ---
// GET the URL and append the body to `out`. Returns true on 2xx response.
// `error` (optional) receives a short diagnostic on failure.
bool hw_http_get_string(const char *url, std::string &out, std::string *error = nullptr);

// Flexible HTTP call. Set `method` to "GET", "POST", etc. A `body` != nullptr
// is sent verbatim with `content_type`. `auth_header` (e.g. "Bearer abc123")
// is added as the Authorization header when non-null.
//
// On completion `status_code` (optional) receives the HTTP status regardless
// of success. The function returns true only on 2xx responses; non-2xx sets
// `error` to "HTTP <code>" and returns false but still populates `out` so
// callers can inspect error bodies if they care.
bool hw_http_request(const char *url,
                     const char *method,
                     const char *body,
                     size_t body_len,
                     const char *content_type,
                     const char *auth_header,
                     std::string &out,
                     int *status_code = nullptr,
                     std::string *error = nullptr);

// Quick internet reachability test. Opens a TCP connection to `host:port` with
// `timeout_ms`, returns true on success. On success `elapsed_ms` (optional)
// receives the round-trip time to connect. On failure `error` (optional) gets
// a short diagnostic.
bool hw_ping_internet(const char *host, uint16_t port, uint32_t timeout_ms,
                      uint32_t *elapsed_ms, std::string *error);

// The default probe: Cloudflare DNS (1.1.1.1:53) with a 3 s timeout — always
// up, firewall-friendly, and does not require ICMP.
inline bool hw_ping_internet(uint32_t *elapsed_ms = nullptr,
                             std::string *error = nullptr)
{
    return hw_ping_internet("1.1.1.1", 53, 3000, elapsed_ms, error);
}

// --- BLE ---
bool hw_get_bt_enable();
void hw_set_bt_enable(bool en);

// --- BLE keyboard ---
const char *hw_get_ble_kb_name();
void hw_set_ble_kb_char(const char *c);
bool hw_get_ble_kb_connected();
std::string hw_get_ble_kb_peer_address();
void hw_disconnect_ble_kb();
void hw_set_ble_key(media_key_value_t key);
