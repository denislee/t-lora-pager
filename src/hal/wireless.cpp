/**
 * @file      wireless.cpp
 * @brief     WiFi + BLE + BLE keyboard.
 */
#include "wireless.h"
#include "internal.h"
#include "board_config.h"
#include "system.h"
#include "core/system_hooks.h"
#include "ca_certs.h"   // P4.27 root CA bundle
#include "ble_stack.h"  // hal::ble::DEVICE_NAME

#ifdef ARDUINO
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_wifi.h>      // esp_wifi_set_ps() for fine-grained power-save control
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_heap_caps.h>  // heap_caps_get_largest_free_block() for StringSink
#include "task_util.h"
// board_config.h defines USING_BLE_KEYBOARD for every supported board, so the
// BLE keyboard code below is guarded on ARDUINO alone.
#if !defined(USING_BLE_KEYBOARD)
#error "USING_BLE_KEYBOARD is expected on every board (see hal/board_config.h)"
#endif
#include "ble_stack.h"
#include "ble_hid.h"

// iOS is strict about Apple's Accessory Design Guidelines for BLE HID; the
// keepalive task applies the compliant connection parameters
// (hal::ble::apply_idle_conn_params) once a keyboard peer has settled, and
// emits an empty HID report every 25 s so iOS sees the peripheral as "live"
// and does not park the connection.
static TaskHandle_t ble_kb_keepalive_handle = nullptr;

static void ble_kb_keepalive_task(void *)
{
    bool was_connected = false;
    uint32_t last_ka_ms = 0;
    for (;;) {
        // PB.12/Lever3: park during fake sleep — the keyboard rail is cut
        // while the display is off, so no keystrokes can be sent. Blocking
        // here removes the 1 Hz wakes. was_connected is cleared so conn-params
        // re-apply on the next real connect after wake. ui_resume_timers()
        // calls hw_ble_kb_task_notify_wake() to unblock immediately on wake.
        if (hal::park_if_fake_sleep()) {
            was_connected = false;
            continue;
        }
        // "Connected" = a peer subscribed to HID input, so a companion-only
        // connection gets neither the keepalive nor the HID conn params.
        bool now_connected = hal::ble_hid::connected();
        if (now_connected && !was_connected) {
            // Give the iPhone a moment to finish service discovery / pairing
            // before we push a parameter update.
            vTaskDelay(pdMS_TO_TICKS(1500));
            hal::ble::apply_idle_conn_params(hal::ble_hid::peer_conn_handle());
            last_ka_ms = millis();
        }
        if (now_connected && (millis() - last_ka_ms) >= 25000) {
            hal::ble_hid::release_all();
            last_ka_ms = millis();
        }
        was_connected = now_connected;
        // P4.14: 1 Hz while connected (keepalive timing + conn-param watch);
        // 5 s cadence while disconnected — the 1 Hz was pointless overhead.
        vTaskDelay(pdMS_TO_TICKS(now_connected ? 1000 : 5000));
    }
}

#define WIFI_PREFS_NS     "wifi_cred"
#define WIFI_PREFS_COUNT  "count"
#define WIFI_MAX_SAVED    16

// NVS key names are capped at 15 chars; "s0".."s15" / "p0".."p15" stay well
// under that, so max-saved could be raised later if needed.
static void wifi_key_ssid(int i, char out[8]) { snprintf(out, 8, "s%d", i); }
static void wifi_key_pass(int i, char out[8]) { snprintf(out, 8, "p%d", i); }

static int wifi_saved_count_ro(Preferences &p)
{
    int n = p.getUChar(WIFI_PREFS_COUNT, 0);
    if (n > WIFI_MAX_SAVED) n = WIFI_MAX_SAVED;
    return n;
}

static bool wifi_load_at(int i, std::string &ssid, std::string &password)
{
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return false;
    char sk[8], pk[8];
    wifi_key_ssid(i, sk);
    wifi_key_pass(i, pk);
    String s = p.getString(sk, "");
    String pw = p.getString(pk, "");
    p.end();
    if (s.length() == 0) return false;
    ssid = s.c_str();
    password = pw.c_str();
    return true;
}

// Most recent saved credential (index 0). Returns false if nothing is saved.
static bool wifi_load_saved(std::string &ssid, std::string &password)
{
    return wifi_load_at(0, ssid, password);
}
#else
#include <cstring>
#endif

// --- WiFi enable / state ----------------------------------------------

bool hw_get_wifi_enable() { return user_setting.wifi_enable; }
void hw_set_wifi_enable(bool en) {
    user_setting.wifi_enable = en;
#ifdef ARDUINO
    if (en) {
        WiFi.mode(WIFI_STA);
        // Make the Arduino default explicit — determinism, not a behaviour change.
        // Without this call the stack uses WIFI_PS_MIN_MODEM by default, but
        // keeping it implicit means hw_wifi_powersave_active() cannot reliably
        // restore MIN after a fake-sleep wake (the baseline would be unknown).
        WiFi.setSleep(WIFI_PS_MIN_MODEM);
        hw_wifi_reconnect_saved();
    } else {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
#endif
}

void hw_wifi_reconnect_saved()
{
#ifdef ARDUINO
    if (WiFi.isConnected()) return;
    std::string s, pw;
    if (wifi_load_saved(s, pw)) {
        WiFi.begin(s.c_str(), pw.c_str());
    }
#endif
}

// PB.2 — WiFi power-save wrappers called by hw_power_down_all / hw_power_up_all.
//
// While the device is in fake sleep the LVGL timer that drives the 60 s Telegram
// poll is frozen, so there is no inbound-latency consumer to worry about.
// MAX_MODEM lets the RF front-end sleep for the full listen interval instead of
// waking every DTIM beacon, saving an estimated 1–5 mA while associated.
// The guard on hw_get_wifi_connected() means we touch the stack only when there
// is an active association — if WiFi is off or disconnected the calls are no-ops.
void hw_wifi_powersave_sleep()
{
#ifdef ARDUINO
    if (hw_get_wifi_connected()) {
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
#endif
}

void hw_wifi_powersave_active()
{
#ifdef ARDUINO
    if (hw_get_wifi_connected()) {
        // Restore the explicit baseline set in hw_set_wifi_enable().
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
#endif
}

void hw_get_wifi_ssid(std::string &param)
{
#ifdef ARDUINO
    param = WiFi.isConnected() ?  WiFi.SSID().c_str() : "N.A";
#else
    param = "NO CONFIG";
#endif
}

void hw_get_ip_address(std::string &param)
{
#ifdef ARDUINO
    if (WiFi.isConnected()) {
        param = WiFi.localIP().toString().c_str();
        return;
    }
#endif
    param = "N.A";
}

int16_t hw_get_wifi_rssi()
{
#ifdef ARDUINO
    if (WiFi.isConnected()) {
        return (WiFi.RSSI());
    }
#endif
    return -99;
}

int16_t hw_set_wifi_scan()
{
#ifdef ARDUINO
    log_d("hw_set_wifi_scan");
    return  WiFi.scanNetworks(true);
#endif
    return 0;
}

bool hw_get_wifi_scanning()
{
#ifdef ARDUINO
    return WiFi.getStatusBits() & WIFI_SCANNING_BIT ;
#endif
    return false;
}

void hw_get_wifi_scan_result(std::vector<wifi_scan_params_t> &list)
{
    list.clear();
#ifdef ARDUINO
    int16_t nums = WiFi.scanComplete();
    if (nums < 0) {
        log_d("Nothing network found. return code : %d", nums);
        return;
    } else {
        log_d("find %d network", nums);
    }
    // uint8_t networkItem, String &ssid, uint8_t &encryptionType, int32_t &RSSI, uint8_t *&BSSID, int32_t &channel
    wifi_scan_params_t param;
    for (int i = 0; i < nums; ++i) {
        String ssid;
        uint8_t encryptionType;
        int32_t rssi;
        uint8_t *BSSID;
        int32_t channel;
        WiFi.getNetworkInfo(i, ssid, encryptionType, rssi, BSSID, channel);
        log_d("SSID:%s RSSI:%d", ssid.c_str(), rssi);
        param.authmode = encryptionType;
        param.ssid = ssid.c_str();
        param.rssi = rssi;
        param.channel = channel;
        list.push_back(param);
    }
#else
    wifi_scan_params_t param;
    param.authmode = 1;
    param.ssid = "LilyGo-AABB0";
    param.rssi = -10;
    param.channel = 0;
    list.push_back(param);
#endif
}

void hw_set_wifi_connect(wifi_conn_params_t &params)
{
    // P4.28: never log the password — SSID only.
    log_d("hw_set_wifi_connect ssid:<%s>", params.ssid.c_str());
#ifdef ARDUINO
    String ssid = params.ssid.c_str();
    String password = params.password.c_str();
    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
    }
    WiFi.disconnect(false, false);
    WiFi.begin(ssid, password);
    // Saving is deferred to hw_wifi_add_saved(), called by the UI only after
    // association succeeds — that way bad passwords don't pollute favorites.
#endif
}

bool hw_get_wifi_connected()
{
#ifdef ARDUINO
    return WiFi.isConnected();
#endif
    return false;
}

void hw_set_wifi_disconnect()
{
#ifdef ARDUINO
    WiFi.disconnect(false, true);
#endif
}

bool hw_wifi_has_saved()
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return false;
    int n = wifi_saved_count_ro(p);
    p.end();
    return n > 0;
#else
    return false;
#endif
}

bool hw_wifi_get_saved_password(const std::string &ssid, std::string &password)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return false;
    int n = wifi_saved_count_ro(p);
    for (int i = 0; i < n; ++i) {
        char sk[8], pk[8];
        wifi_key_ssid(i, sk);
        wifi_key_pass(i, pk);
        String s = p.getString(sk, "");
        if (s.length() == 0) continue;
        if (ssid == s.c_str()) {
            String pw = p.getString(pk, "");
            password = pw.c_str();
            p.end();
            return true;
        }
    }
    p.end();
    return false;
#else
    (void)ssid; (void)password;
    return false;
#endif
}

void hw_wifi_get_saved_list(std::vector<std::string> &ssids)
{
    ssids.clear();
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, true)) return;
    int n = wifi_saved_count_ro(p);
    for (int i = 0; i < n; ++i) {
        char sk[8];
        wifi_key_ssid(i, sk);
        String s = p.getString(sk, "");
        if (s.length() > 0) ssids.emplace_back(s.c_str());
    }
    p.end();
#endif
}

void hw_wifi_add_saved(const std::string &ssid, const std::string &password)
{
#ifdef ARDUINO
    if (ssid.empty()) return;

    // Read the existing list, dedup the incoming SSID, then prepend it.
    // Keeps most-recent at index 0 so hw_wifi_get_saved_ssid() stays useful
    // for auto-reconnect on boot.
    std::vector<std::pair<std::string, std::string>> kept;
    kept.reserve(WIFI_MAX_SAVED);
    kept.emplace_back(ssid, password);

    Preferences p;
    if (!p.begin(WIFI_PREFS_NS, false)) return;
    int n = wifi_saved_count_ro(p);
    for (int i = 0; i < n && (int)kept.size() < WIFI_MAX_SAVED; ++i) {
        char sk[8], pk[8];
        wifi_key_ssid(i, sk);
        wifi_key_pass(i, pk);
        String s = p.getString(sk, "");
        if (s.length() == 0) continue;
        if (ssid == s.c_str()) continue;  // dedup — we just prepended it
        String pw = p.getString(pk, "");
        kept.emplace_back(s.c_str(), pw.c_str());
    }

    for (size_t i = 0; i < kept.size(); ++i) {
        char sk[8], pk[8];
        wifi_key_ssid((int)i, sk);
        wifi_key_pass((int)i, pk);
        p.putString(sk, kept[i].first.c_str());
        p.putString(pk, kept[i].second.c_str());
    }
    // Clear any trailing slots that used to be populated.
    for (size_t i = kept.size(); i < (size_t)n + 1 && i < WIFI_MAX_SAVED; ++i) {
        char sk[8], pk[8];
        wifi_key_ssid((int)i, sk);
        wifi_key_pass((int)i, pk);
        p.remove(sk);
        p.remove(pk);
    }
    p.putUChar(WIFI_PREFS_COUNT, (uint8_t)kept.size());
    p.end();
#else
    (void)ssid; (void)password;
#endif
}

void hw_wifi_forget()
{
#ifdef ARDUINO
    Preferences p;
    if (p.begin(WIFI_PREFS_NS, false)) {
        p.clear();
        p.end();
    }
    WiFi.disconnect(false, true);
#endif
}

// --- HTTP --------------------------------------------------------------

#ifdef ARDUINO
namespace {

// ---------------------------------------------------------------------------
// P4.15 (TLS session resumption) is NOT implemented, and cannot be on this
// framework. Verified against framework-arduinoespressif32@3.20014.231204
// (arduino-esp32 2.0.14): WiFiClientSecure exposes no setSession()/getSession(),
// and start_ssl_client() calls mbedtls_ssl_setup() immediately before
// mbedtls_ssl_handshake() with no hook in between — so a session seeded before
// connect() is discarded by the setup call. Saving sessions without being able
// to restore them buys nothing and costs static RAM, so no cache is kept here.
//
// Unblock path, in preference order:
//   (a) upgrade the platform to arduino-esp32 3.x, whose NetworkClientSecure
//       exposes setSession(), then seed from a small per-host LRU cache; or
//   (b) vendor a patched ssl_client.cpp that accepts an mbedtls_ssl_session*
//       and calls mbedtls_ssl_set_session() between setup and handshake.
// Until then every HTTPS request pays a full handshake — which is exactly why
// P4.16's cadence backoff and the hub-first plain-HTTP LAN path matter.
// HTTPClient::setReuse(true) below is kept: it is free and helps whenever a
// single HTTPClient makes more than one request (the redirect chain).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// P4.27: enable TLS certificate validation.
//
// Every HTTPS connection this firmware makes used to run setInsecure(), so the
// notes-sync token and — worse — the Telegram bot token in the Authorization
// header travelled over connections any on-path attacker could impersonate with
// a self-signed cert.
//
// The full Mozilla root bundle is used rather than a pinned per-host root
// table. Verified 2026-08-22, the hosts do not share a root and they rotate:
// api.github.com chains to Sectigo Root E46, while raw.githubusercontent.com
// and api.open-meteo.com chain to an ISRG (Let's Encrypt) root. A pinned table
// that guesses one of those wrong fails closed in the field with no recovery
// short of a reflash; ~55 KB of flash removes that entire failure class.
//
// There is no setInsecure() fallback on failure — that would reintroduce the
// downgrade this fixes. A validation failure surfaces as a failed request.
static void setup_secure(WiFiClientSecure &sec)
{
    sec.setCACertBundle(kRootCaBundle);
}

struct HttpClients {
    WiFiClientSecure secure;
    WiFiClient plain;
    HTTPClient http;
};

// Follows redirects; returns true with `http` begin()'d on the final 2xx.
static bool http_open(HttpClients &c, const char *url, std::string *error)
{
    if (!WiFi.isConnected()) {
        if (error) *error = "WiFi not connected.";
        return false;
    }
    setup_secure(c.secure);                // P4.27: cert validation
    c.http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    c.http.setTimeout(5000);
    c.http.setConnectTimeout(5000);
    c.http.setUserAgent("LilyGoLib/1.0");
    c.http.setReuse(true);                 // reuse the socket across redirects

    bool ok;
    String u = url;
    if (u.startsWith("https://")) {
        ok = c.http.begin(c.secure, u);
    } else {
        ok = c.http.begin(c.plain, u);
    }
    if (!ok) {
        if (error) *error = "begin() failed.";
        return false;
    }
    int code = c.http.GET();
    if (code < 200 || code >= 300) {
        if (error) {
            char buf[64];
            snprintf(buf, sizeof(buf), "HTTP %d", code);
            *error = buf;
        }
        c.http.end();
        return false;
    }
    return true;
}

// P7.42: HTTPClient::writeToStream() wants a Stream (not a Print), so this is
// a write-only Stream that appends the body straight onto the caller's
// std::string: one copy of the body instead of getString()'s StreamString plus
// the append. Growth is explicit and checked: src/ builds with -fno-exceptions,
// so an allocation std::string could not satisfy would abort. A write that
// can't grow returns 0, which writeToStream reports as HTTPC_ERROR_STREAM_WRITE
// (the same short-write path a failed StreamString realloc took before).
class StringSink : public Stream {
public:
    explicit StringSink(std::string &s) : s_(s) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *buf, size_t n) override
    {
        if (n == 0) return 0;
        if (s_.capacity() - s_.size() < n && !grow(s_.size() + n)) return 0;
        s_.append((const char *)buf, n);
        return n;
    }
    // Make room for `need` bytes in total: geometric while streaming, exact
    // for a known Content-Length. False (nothing reserved) if the heap can't
    // take it.
    bool grow(size_t need, bool exact = false)
    {
        size_t cap = s_.capacity();
        size_t want = (!exact && cap * 2 > need) ? cap * 2 : need;   // geometric
        if (!fits(want)) want = need;                    // exact as a fallback
        if (!fits(want)) return false;
        s_.reserve(want);
        return true;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
private:
    static bool fits(size_t bytes)
    {
        // +64: allocator header and the terminator std::string adds.
        return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > bytes + 64;
    }
    std::string &s_;
};

// Appends the response body to `out`, the way getString() used to fill it:
// nothing when the server announced Content-Length: 0, otherwise whatever
// arrived (identity or chunked; writeToStream decodes chunked itself). A read
// error keeps the partial body and is only logged, exactly as getString()
// ignored writeToStream's result; callers judge success by the status code.
static void http_read_body(HTTPClient &http, std::string &out)
{
    int size = http.getSize();          // -1: no Content-Length (e.g. chunked)
    if (size == 0) return;
    StringSink sink(out);
    if (size > 0 && !sink.grow(out.size() + (size_t)size, true)) {
        log_w("[http] no heap for a %d B body", size);
        return;                         // getString(): reserve failed -> ""
    }
    int r = http.writeToStream(&sink);
    if (r < 0) {
        log_w("[http] body read error %d (%s), kept %u B", r,
              HTTPClient::errorToString(r).c_str(), (unsigned)out.size());
    }
}
} // namespace
#endif

bool hw_ping_internet(const char *host, uint16_t port, uint32_t timeout_ms,
                      uint32_t *elapsed_ms, std::string *error)
{
#ifdef ARDUINO
    if (!WiFi.isConnected()) {
        if (error) *error = "WiFi not connected.";
        return false;
    }
    WiFiClient client;
    client.setTimeout(timeout_ms / 1000 ? timeout_ms / 1000 : 1);
    uint32_t t0 = millis();
    int rc = client.connect(host, port, timeout_ms);
    uint32_t dt = millis() - t0;
    if (!rc) {
        client.stop();
        if (error) *error = "Unreachable.";
        return false;
    }
    client.stop();
    if (elapsed_ms) *elapsed_ms = dt;
    return true;
#else
    (void)host; (void)port; (void)timeout_ms;
    if (elapsed_ms) *elapsed_ms = 0;
    if (error) *error = "Not supported on emulator.";
    return false;
#endif
}

bool hw_http_get_string(const char *url, std::string &out, std::string *error)
{
#ifdef ARDUINO
    HttpClients c;
    if (!http_open(c, url, error)) return false;
    http_read_body(c.http, out);
    c.http.end();
    return true;
#else
    (void)url; (void)out; (void)error;
    return false;
#endif
}

bool hw_http_request(const char *url,
                     const char *method,
                     const char *body,
                     size_t body_len,
                     const char *content_type,
                     const char *auth_header,
                     std::string &out,
                     int *status_code,
                     std::string *error)
{
#ifdef ARDUINO
    if (!WiFi.isConnected()) {
        if (error) *error = "WiFi not connected.";
        if (status_code) *status_code = 0;
        return false;
    }
    HttpClients c;
    setup_secure(c.secure);                // P4.27: cert validation
    c.http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    c.http.setTimeout(5000);
    c.http.setConnectTimeout(5000);
    c.http.setUserAgent("LilyGoLib/1.0");
    c.http.setReuse(true);                 // reuse the socket across redirects

    String u = url;
    bool begun = u.startsWith("https://") ? c.http.begin(c.secure, u)
                                           : c.http.begin(c.plain, u);
    if (!begun) {
        if (error) *error = "begin() failed.";
        if (status_code) *status_code = 0;
        return false;
    }
    if (auth_header && *auth_header) {
        c.http.addHeader("Authorization", auth_header);
    }
    if (content_type && *content_type && body) {
        c.http.addHeader("Content-Type", content_type);
    }

    int code;
    const char *m = method && *method ? method : "GET";
    if (body) {
        code = c.http.sendRequest(m, (uint8_t *)body, body_len);
    } else {
        code = c.http.sendRequest(m);
    }
    if (status_code) *status_code = code;

    // Always capture the body so callers can read error payloads too.
    http_read_body(c.http, out);
    c.http.end();

    if (code < 200 || code >= 300) {
        if (error) {
            char buf[48];
            snprintf(buf, sizeof(buf), "HTTP %d", code);
            *error = buf;
        }
        return false;
    }
    return true;
#else
    (void)url; (void)method; (void)body; (void)body_len;
    (void)content_type; (void)auth_header; (void)out;
    if (status_code) *status_code = 0;
    if (error) *error = "Not supported on emulator.";
    return false;
#endif
}

// --- WiFi reconnect supervisor (P4.17) --------------------------------
//
// Call hw_wifi_supervise(millis()) from the main loop (factory.ino / loop())
// at any cadence >= 1 Hz. The function is self-throttled: it tracks its own
// retry timestamps and does nothing if it is not yet time to act.
//
// State machine:
//   IDLE       — connected or wifi_enable just turned false. No action.
//   BACKOFF    — disconnected; retry hw_wifi_reconnect_saved() on exponential
//                backoff 30 s -> 60 s -> 300 s (capped). After 5 consecutive
//                failures, kill WiFi entirely and enter LONG_BACKOFF.
//   LONG_BACKOFF — WiFi modem off; retry after 15 min, then restart from
//                  the 30 s backoff schedule.
//
// hw_wifi_reconnect_saved() is the existing function that drives WiFi.begin()
// from the saved credential. setAutoReconnect remains false — we drive it.

void hw_wifi_supervise(uint32_t now_ms)
{
#ifdef ARDUINO
    static uint32_t s_next_ms       = 0;
    static uint32_t s_backoff_ms    = 30000u;  // 30 s initial
    static int      s_fails         = 0;
    static bool     s_armed         = false;
    static bool     s_off_for_backoff = false;

    if (!user_setting.wifi_enable) {
        // WiFi disabled by the user — reset everything.
        s_armed = false; s_fails = 0;
        s_backoff_ms = 30000u; s_off_for_backoff = false;
        return;
    }

    if (hw_get_wifi_connected()) {
        // Successful association — reset backoff state.
        if (s_fails || s_off_for_backoff || s_armed) {
            log_i("[wifi_sv] connected — backoff reset");
        }
        s_armed = false; s_fails = 0;
        s_backoff_ms = 30000u; s_off_for_backoff = false;
        return;
    }

    // WiFi should be enabled but we are not connected.
    if (!s_armed) {
        // Arm the first retry interval.
        s_next_ms = now_ms + s_backoff_ms;
        s_armed = true;
        return;
    }

    // Not yet time to retry?
    if ((int32_t)(now_ms - s_next_ms) < 0) return;

    // --- time to act ---

    if (s_off_for_backoff) {
        // Restore the modem after the long backoff and try once.
        log_i("[wifi_sv] long backoff expired — restoring modem");
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_MIN_MODEM);
        s_off_for_backoff = false;
        s_fails = 0;
        s_backoff_ms = 30000u;
        // Arm immediately for the first normal retry.
        s_next_ms = now_ms;
        return;
    }

    log_i("[wifi_sv] retry attempt %d (interval %u s)", s_fails + 1, s_backoff_ms / 1000u);
    hw_wifi_reconnect_saved();
    s_fails++;

    if (s_fails >= 5) {
        // Five consecutive misses — kill the modem for 15 min.
        log_w("[wifi_sv] 5 failures — WiFi OFF for 15 min");
        WiFi.mode(WIFI_OFF);
        s_off_for_backoff = true;
        s_next_ms = now_ms + 15u * 60u * 1000u;
        s_fails = 0;
    } else {
        // Exponential backoff: 30 s -> 60 s -> 300 s (cap).
        if (s_backoff_ms < 60000u)       s_backoff_ms = 60000u;
        else if (s_backoff_ms < 300000u) s_backoff_ms = 300000u;
        s_next_ms = now_ms + s_backoff_ms;
    }
#else
    (void)now_ms;
#endif
}

// --- BLE (UART) -------------------------------------------------------

bool hw_get_bt_enable() { return user_setting.bt_enable; }
void hw_set_bt_enable(bool en) {
    user_setting.bt_enable = en;
    if (en) {
        hw_set_ble_kb_enable();
    } else {
        hw_set_ble_kb_disable();
    }
}

// --- BLE keyboard ------------------------------------------------------

const char *hw_get_ble_kb_name()
{
    return hal::ble::DEVICE_NAME;
}

void hw_set_ble_kb_enable()
{
#ifdef ARDUINO
    // ble_stack inits NimBLE once (HID + PagerLink on one server) and only
    // (re)starts advertising on later calls.
    if (!hal::ble::enable()) {
        return;
    }
    if (!ble_kb_keepalive_handle) {
        xTaskCreate(ble_kb_keepalive_task, "ble_kb_ka", 3072, nullptr,
                    tskIDLE_PRIORITY + 1, &ble_kb_keepalive_handle);
    }
#endif
}

void hw_set_ble_kb_disable()
{
#ifdef ARDUINO
    if (ble_kb_keepalive_handle) {
        vTaskDelete(ble_kb_keepalive_handle);
        ble_kb_keepalive_handle = nullptr;
    }
    // Stops advertising and drops peers; NimBLE stays initialised because it
    // leaks on every deinit/init cycle (IOS_COMPANION_PLAN.md §5.1).
    hal::ble::disable();
    log_d("Disable ble devices");
#endif
}

void hw_set_ble_kb_char(const char *c)
{
#ifdef ARDUINO
    hal::ble_hid::type(c);
#endif
}

bool hw_get_ble_kb_connected()
{
#ifdef ARDUINO
    return hal::ble_hid::connected();
#else
    return false;
#endif
}

std::string hw_get_ble_kb_peer_address()
{
#ifdef ARDUINO
    if (hal::ble_hid::connected()) {
        return hal::ble::peer_address(hal::ble_hid::peer_conn_handle());
    }
#endif
    return "";
}

void hw_disconnect_ble_kb()
{
#ifdef ARDUINO
    if (hal::ble_hid::connected()) {
        hal::ble::disconnect(hal::ble_hid::peer_conn_handle());
    }
#endif
}

void hw_set_ble_key(media_key_value_t key)
{
#ifdef ARDUINO
    switch (key) {
    case MEDIA_VOLUME_UP:
        hal::ble_hid::media_key(hal::ble_hid::MEDIA_VOLUME_UP);
        break;
    case MEDIA_VOLUME_DOWN:
        hal::ble_hid::media_key(hal::ble_hid::MEDIA_VOLUME_DOWN);
        break;
    case MEDIA_PLAY_PAUSE:
        hal::ble_hid::media_key(hal::ble_hid::MEDIA_PLAY_PAUSE);
        break;
    case MEDIA_NEXT:
        hal::ble_hid::media_key(hal::ble_hid::MEDIA_NEXT_TRACK);
        break;
    case MEDIA_PREVIOUS:
        hal::ble_hid::media_key(hal::ble_hid::MEDIA_PREVIOUS_TRACK);
        break;
    default:
        return;
    }
#endif
}

// PB.12/Lever3: unblock the BLE keepalive task when leaving fake sleep.
// Called from ui_resume_timers() alongside the LVGL/keyboard/rotary kicks.
// Safe from any task context — a give while the task is running simply
// leaves the notification pending for the next take. No-op on the emulator.
void hw_ble_kb_task_notify_wake()
{
#ifdef ARDUINO
    hal::notify_task(ble_kb_keepalive_handle);
#endif
}
