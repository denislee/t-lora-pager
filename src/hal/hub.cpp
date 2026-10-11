/**
 * @file      hub.cpp
 * @brief     Local hub config storage. See hub.h.
 */
#include "hub.h"
#include "wireless.h"
#include "str_encode.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace hal {

namespace {
// Everything in here backs the NVS-backed hardware implementation; the
// emulator branches below return fixed stub values.
#ifdef ARDUINO
constexpr const char *NS = "hub";
constexpr const char *KEY_URL = "url";
constexpr const char *KEY_ENABLED = "enabled";

// Legacy slot — hub URL used to live under the "weather" namespace before this
// module existed. See hub.h for the migration contract.
constexpr const char *LEGACY_NS = "weather";
constexpr const char *LEGACY_KEY = "hub_url";

// Whitespace-trim and drop any trailing '/' (interleaved with whitespace or
// not), so callers can append "/api/..." directly.
std::string trim_url(const std::string &in)
{
    std::string s = trim(in);
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
        s = trim(s);
    }
    return s;
}

// One-shot legacy migration: if the new slot is empty but the old one has a
// value, copy it across, force the toggle ON (preserving prior behavior —
// having set the URL meant the user wanted hub-first), and delete the legacy
// key. Safe to call repeatedly: once the legacy slot is gone, this is a no-op.
void maybe_migrate_legacy()
{
    // Runs at most once per boot. The migration is idempotent and, once the
    // legacy slot is resolved, permanently pointless — yet every hub getter
    // used to reopen NVS to re-check it forever. The guard collapses that to a
    // single check; a benign race (two threads both seeing false) just runs the
    // idempotent check twice. s_checked is only latched once we've positively
    // determined the migration state, so a transient begin() failure retries.
    static bool s_checked = false;
    if (s_checked) return;

    Preferences neu;
    if (!neu.begin(NS, true)) return;   // transient open failure — retry later
    bool have_new = neu.isKey(KEY_URL);
    neu.end();
    if (have_new) { s_checked = true; return; }

    Preferences old;
    if (!old.begin(LEGACY_NS, false)) return;
    String legacy = old.getString(LEGACY_KEY, "");
    if (legacy.length() == 0) {
        old.end();
        s_checked = true;   // no legacy value exists — nothing to migrate, ever
        return;
    }
    old.remove(LEGACY_KEY);
    old.end();

    Preferences w;
    if (!w.begin(NS, false)) return;    // write failed — retry migration later
    w.putString(KEY_URL, legacy);
    w.putBool(KEY_ENABLED, true);
    w.end();
    s_checked = true;
}

// --- RAM cache of the NVS-backed config ---------------------------------
// NVS is only written through the hub_set_* setters below, so once loaded this
// cache is authoritative for the process. Getters run on many threads (the
// status-bar timer, storage tasks, the weather/chat/telegram workers), so
// access is serialised through a mutex. This collapses the 1-3 NVS opens every
// hub-touching request used to pay down to a single microsecond-scale lock.
SemaphoreHandle_t cfg_mutex()
{
    // C++11 thread-safe static init, matching the notes_crypto prewarm idiom.
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
struct CfgLock {
    CfgLock()  { xSemaphoreTake(cfg_mutex(), portMAX_DELAY); }
    ~CfgLock() { xSemaphoreGive(cfg_mutex()); }
    CfgLock(const CfgLock &) = delete;
    CfgLock &operator=(const CfgLock &) = delete;
};
bool        s_cfg_loaded  = false;
bool        s_cfg_enabled = false;
std::string s_cfg_url;      // trimmed; "" when unset

// Populate the cache from NVS, running the one-shot legacy migration first.
// Caller holds cfg_mutex().
void cfg_ensure_locked()
{
    if (s_cfg_loaded) return;
    maybe_migrate_legacy();
    s_cfg_enabled = false;
    s_cfg_url.clear();
    Preferences p;
    if (p.begin(NS, true)) {
        s_cfg_enabled = p.getBool(KEY_ENABLED, false);
        String h = p.getString(KEY_URL, "");
        p.end();
        s_cfg_url = trim_url(h.c_str());
    }
    s_cfg_loaded = true;
}
// Force a reload on the next read. Setters call this after writing NVS instead
// of poking individual fields, so a set of one field can never leave the other
// stale/unloaded.
void cfg_invalidate()
{
    CfgLock lk;
    s_cfg_loaded = false;
}

// --- Cached reachability verdict ----------------------------------------
// The status-bar timer TCP-probes the hub every ~10 s on a background task and
// publishes the result here; hot paths read this cached verdict instead of
// doing their own blocking connect() on the UI thread. Plain volatiles (no
// barrier) mirror how the status-bar probe already shares its result — a rare
// torn read costs at most one request taking the direct fallback path.
volatile bool     s_reach_known = false;
volatile bool     s_reach_value = false;
volatile uint32_t s_reach_ms    = 0;
#endif

} // namespace

std::string hub_get_url_raw()
{
#ifdef ARDUINO
    CfgLock lk;
    cfg_ensure_locked();
    return s_cfg_url;
#else
    return "";
#endif
}

bool hub_get_enabled_pref()
{
#ifdef ARDUINO
    CfgLock lk;
    cfg_ensure_locked();
    return s_cfg_enabled;
#else
    return false;
#endif
}

bool hub_is_enabled()
{
#ifdef ARDUINO
    CfgLock lk;
    cfg_ensure_locked();
    return s_cfg_enabled && !s_cfg_url.empty();
#else
    return false;
#endif
}

std::string hub_get_url()
{
#ifdef ARDUINO
    // One locked read of both fields (this used to take the mutex 3x).
    CfgLock lk;
    cfg_ensure_locked();
    return s_cfg_enabled ? s_cfg_url : std::string();
#else
    return "";
#endif
}

void hub_set_enabled(bool enabled)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NS, false)) return;
    p.putBool(KEY_ENABLED, enabled);
    p.end();
    cfg_invalidate();
#else
    (void)enabled;
#endif
}

void hub_set_url(const char *url)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NS, false)) return;
    if (url && *url) {
        p.putString(KEY_URL, url);
    } else {
        p.remove(KEY_URL);
    }
    p.end();
    cfg_invalidate();
#else
    (void)url;
#endif
}

HalError hub_upload_note(const char *name, const uint8_t *bytes, size_t len)
{
#ifdef ARDUINO
    if (!name || !*name)                return HalError::InvalidArgument;
    std::string base = hub_get_url();
    if (base.empty())                   return HalError::HubDisabled;
    if (!hw_get_wifi_connected())       return HalError::WifiOffline;

    std::string b64;
    if (!base64_encode(bytes, len, b64)) return HalError::InternalError;

    std::string body;
    body.reserve(64 + b64.size());
    body += "{\"name\":\"";        body += json_escape(name); body += "\",";
    body += "\"content_b64\":\""; body += b64;                   body += "\"}";

    std::string url = base + "/api/notes/upload";
    std::string resp;
    int code = 0;
    std::string terr;
    bool ok = hw_http_request(url.c_str(), "POST",
                              body.c_str(), body.size(),
                              "application/json",
                              nullptr, resp, &code, &terr);
    if (!ok)            return HalError::HubUnreachable;
    if (code == 401 || code == 403) return HalError::Unauthorized;
    if (code / 100 != 2) return HalError::HttpError;
    return HalError::Ok;
#else
    (void)name; (void)bytes; (void)len;
    return HalError::NotSupported;
#endif
}

bool hub_is_reachable(uint32_t timeout_ms)
{
#ifdef ARDUINO
    std::string url = hub_get_url();   // "" when disabled or unset
    if (url.empty()) return false;
    if (!hw_get_wifi_connected()) return false;

    // `http[s]://host[:port][/...]` — a plain TCP connect to host:port, with
    // the port defaulting from the scheme (http=80, https=443).
    UrlParts u;
    if (!parse_url(url, u)) return false;
    return hw_ping_internet(u.host.c_str(), u.port, timeout_ms, nullptr, nullptr);
#else
    (void)timeout_ms;
    return false;
#endif
}

void hub_note_reachable(bool reachable)
{
#ifdef ARDUINO
    s_reach_value = reachable;
    s_reach_ms    = millis();
    s_reach_known = true;   // publish last: a reader seeing this sees value/ms too
#else
    (void)reachable;
#endif
}

bool hub_last_reachable(uint32_t max_age_ms)
{
#ifdef ARDUINO
    if (!hub_is_enabled()) return false;
    if (!s_reach_known) return false;
    if (millis() - s_reach_ms > max_age_ms) return false;   // stale ⇒ treat as down
    return s_reach_value;
#else
    (void)max_age_ms;
    return false;
#endif
}

} // namespace hal
