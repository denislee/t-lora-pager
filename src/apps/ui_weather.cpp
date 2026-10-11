/**
 * @file      ui_weather.cpp
 * @brief     Single-screen weather viewer.
 *
 * Location (city name + lat/lon) comes from ip-api.com — fast and reliable
 * (~200ms typical). We previously used wttr.in but it routinely took 3-10s,
 * dominating the total fetch time. Once we have coordinates, a single
 * open-meteo call returns current conditions, hourly (24h), and daily (7d)
 * data in one response — no second round-trip.
 *
 * The resolved location is cached in NVS so subsequent opens skip the geo
 * call entirely (one HTTP call instead of two). IP-derived location is
 * stable enough that caching across sessions is fine; a network/IP change
 * will just show a slightly stale city name until the user clears it.
 *
 * The page lays out as a vertical scrollable column: a "24h" header row
 * (with the status text right-aligned), the 3-hourly strip, a "Week" label
 * and the daily strip. Nothing on it is focusable — the strips and cells are
 * passive; tapping anywhere on the panel triggers a refresh.
 *
 * Weather conditions are drawn as small pictorial icons (sun/cloud/rain/etc.)
 * via LV_EVENT_DRAW_MAIN on plain lv_obj_t widgets — no icon font assets to
 * ship, and it composes fine at any panel size.
 *
 * The app-panel is shared across apps, so we are careful never to set a
 * text_font style on the parent: local styles on the panel survive onStop(),
 * and a leaked Montserrat-10 font on the panel would shrink every label in
 * the next app that inherits from it.
 *
 * State reset on onStop (ui_weather_exit):
 *   widgets: hourly_col, daily_col, status_label
 *                                       — nulled (deleted with parent)
 *   job:     s_w_job (HTTPS fetcher + drain ticker, core::OneShotJob)
 *                                       — teardown(): drain killed, a running
 *                                          worker detached (frees its own result)
 * If you add new cached LVGL pointers or a background task, list them
 * above AND extend ui_weather_exit() — see the file header note about
 * panel-inherited styles; the same hazard applies to leaked timers.
 */
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../hal/system.h"
#include "../hal/str_encode.h"
#include "../core/app_manager.h"
#include "../core/one_shot_job.h"
#include "app_registry.h"
#include "settings_internal.h"  // weather_city_match + the weather_* decls
#include <memory>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#ifdef ARDUINO
#include <Preferences.h>
extern "C" {
#include "cJSON.h"
}
#endif

// Defined at the bottom of this file (also used by Settings » Weather).
// "" = auto (IP geolocation); a missing key = WEATHER_DEFAULT_CITY.
std::string weather_get_user_city();

namespace {

enum IconKind {
    ICON_UNKNOWN = 0,
    ICON_SUN,
    ICON_PARTLY,
    ICON_CLOUD,
    ICON_FOG,
    ICON_DRIZZLE,
    ICON_RAIN,
    ICON_SNOW,
    ICON_THUNDER,
};

// ip-api.com's free tier is plain HTTP only; that's fine — no credentials or
// sensitive data in the request, and it saves the TLS handshake. Fields are
// restricted to shrink the response body we have to parse on-device.
#ifdef ARDUINO
static const char *GEO_URL =
    "http://ip-api.com/json/?fields=status,lat,lon,city,regionName,country";
#endif

#define WEATHER_PREFS_NS       "weather"
#define WEATHER_CACHE_VER      1
// If the on-disk forecast is younger than this, the weather app opens with
// zero network calls. Open-meteo's free tier recommends <= 1 req per minute,
// so 15 min leaves plenty of margin while keeping numbers recent enough.
#define WEATHER_FRESH_TTL_SEC  (15 * 60)

// Default city used when the user has not chosen one. Seeded into the NVS
// cache on first use so that without network we still have coordinates to
// feed the forecast call.
static const char   *WEATHER_DEFAULT_CITY = "Sao Paulo";
#ifdef ARDUINO
static const double  WEATHER_DEFAULT_LAT  = -23.5505;
static const double  WEATHER_DEFAULT_LON  = -46.6333;
#endif

static lv_obj_t *hourly_col = nullptr;
static lv_obj_t *daily_col = nullptr;
static lv_obj_t *status_label = nullptr;

// --- async fetch state ----------------------------------------------------
//
// HTTP runs on a core::OneShotJob worker (s_w_job, below weather_fetch) so the
// LVGL thread keeps servicing input (keyboard / encoder / back button) during
// a fetch; the job's drain timer renders the result on the UI thread.

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    menu_show();
}

// Montserrat-10 (what we use here) only covers basic Latin — anything outside
// 0x20–0x7F needs to be stripped or downgraded or it renders as tofu.
#ifdef ARDUINO
static std::string sanitize_ascii(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out.push_back((char)c); i++; continue; }

        uint32_t cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { i++; continue; }

        if (i + extra >= in.size()) break;
        bool ok = true;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)in[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += extra + 1;
        if (!ok) continue;

        if (cp == 0x00B0) continue;  // degree sign — dropped
        if (cp == 0x2013 || cp == 0x2014) { out.push_back('-'); continue; }
        if (cp == 0x2018 || cp == 0x2019) { out.push_back('\''); continue; }
        if (cp == 0x201C || cp == 0x201D) { out.push_back('"'); continue; }

        static const struct { uint32_t cp; char ch; } latin_map[] = {
            {0x00C0,'A'},{0x00C1,'A'},{0x00C2,'A'},{0x00C3,'A'},{0x00C4,'A'},{0x00C5,'A'},
            {0x00C7,'C'},{0x00C8,'E'},{0x00C9,'E'},{0x00CA,'E'},{0x00CB,'E'},
            {0x00CC,'I'},{0x00CD,'I'},{0x00CE,'I'},{0x00CF,'I'},{0x00D1,'N'},
            {0x00D2,'O'},{0x00D3,'O'},{0x00D4,'O'},{0x00D5,'O'},{0x00D6,'O'},
            {0x00D9,'U'},{0x00DA,'U'},{0x00DB,'U'},{0x00DC,'U'},{0x00DD,'Y'},
            {0x00E0,'a'},{0x00E1,'a'},{0x00E2,'a'},{0x00E3,'a'},{0x00E4,'a'},{0x00E5,'a'},
            {0x00E7,'c'},{0x00E8,'e'},{0x00E9,'e'},{0x00EA,'e'},{0x00EB,'e'},
            {0x00EC,'i'},{0x00ED,'i'},{0x00EE,'i'},{0x00EF,'i'},{0x00F1,'n'},
            {0x00F2,'o'},{0x00F3,'o'},{0x00F4,'o'},{0x00F5,'o'},{0x00F6,'o'},
            {0x00F9,'u'},{0x00FA,'u'},{0x00FB,'u'},{0x00FC,'u'},{0x00FD,'y'},{0x00FF,'y'},
        };
        for (auto &m : latin_map) if (m.cp == cp) { out.push_back(m.ch); break; }
    }
    return out;
}
#endif

static void set_status(const char *text, lv_color_t color)
{
    if (!status_label) return;
    lv_label_set_text(status_label, text ? text : "");
    lv_obj_set_style_text_color(status_label, color, 0);
}

#ifdef ARDUINO
// Everything from here to make_cell() is the forecast-grid renderer; only
// render_cache() (hardware fetch path) draws it, so it is hardware-only.

// --- pictorial icon drawing ------------------------------------------------
// Icons are drawn inside a fixed 20x20 widget via LV_EVENT_DRAW_MAIN. Shapes
// are kept intentionally chunky so they read well at this size; coordinates
// are all relative to the widget's top-left.

static void fill_circle(lv_layer_t *layer, int cx, int cy, int r,
                        lv_color_t color)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color;
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(layer, &d, &a);
}

static void stroke_line(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                        int w, lv_color_t color)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.opa = LV_OPA_COVER;
    d.width = w;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(layer, &d);
}

// Three overlapping circles + a flat base = recognizable cloud silhouette.
// ox/oy are the cloud's top-left in screen coords.
static void draw_cloud(lv_layer_t *layer, int ox, int oy, lv_color_t color)
{
    fill_circle(layer, ox + 6,  oy + 7, 4, color);
    fill_circle(layer, ox + 11, oy + 5, 5, color);
    fill_circle(layer, ox + 15, oy + 8, 4, color);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color;
    d.bg_opa = LV_OPA_COVER;
    d.radius = 2;
    lv_area_t base = { ox + 3, oy + 8, ox + 17, oy + 12 };
    lv_draw_rect(layer, &d, &base);
}

static void draw_sun(lv_layer_t *layer, int cx, int cy, int r,
                     lv_color_t color)
{
    stroke_line(layer, cx, cy - r - 3, cx, cy - r - 1, 2, color);
    stroke_line(layer, cx, cy + r + 1, cx, cy + r + 3, 2, color);
    stroke_line(layer, cx - r - 3, cy, cx - r - 1, cy, 2, color);
    stroke_line(layer, cx + r + 1, cy, cx + r + 3, cy, 2, color);
    fill_circle(layer, cx, cy, r, color);
}

static void draw_icon(lv_layer_t *layer, const lv_area_t *a, int kind)
{
    int ox = a->x1, oy = a->y1;
    lv_color_t yellow = lv_color_hex(0xFFD23C);
    lv_color_t grey   = lv_color_hex(0xBDBDBD);
    lv_color_t dgrey  = lv_color_hex(0x8A8A8A);
    lv_color_t blue   = lv_color_hex(0x4AA3FF);
    lv_color_t white  = lv_color_hex(0xFFFFFF);

    switch (kind) {
    case ICON_SUN:
        draw_sun(layer, ox + 10, oy + 10, 5, yellow);
        break;
    case ICON_PARTLY:
        draw_sun(layer, ox + 7, oy + 7, 3, yellow);
        draw_cloud(layer, ox + 2, oy + 6, grey);
        break;
    case ICON_CLOUD:
        draw_cloud(layer, ox, oy + 3, grey);
        break;
    case ICON_FOG:
        stroke_line(layer, ox + 2,  oy + 7,  ox + 17, oy + 7,  2, grey);
        stroke_line(layer, ox + 4,  oy + 11, ox + 15, oy + 11, 2, grey);
        stroke_line(layer, ox + 3,  oy + 15, ox + 16, oy + 15, 2, grey);
        break;
    case ICON_DRIZZLE:
        draw_cloud(layer, ox, oy, grey);
        stroke_line(layer, ox + 6,  oy + 15, ox + 6,  oy + 17, 2, blue);
        stroke_line(layer, ox + 10, oy + 15, ox + 10, oy + 17, 2, blue);
        stroke_line(layer, ox + 14, oy + 15, ox + 14, oy + 17, 2, blue);
        break;
    case ICON_RAIN:
        draw_cloud(layer, ox, oy, grey);
        stroke_line(layer, ox + 6,  oy + 14, ox + 5,  oy + 18, 2, blue);
        stroke_line(layer, ox + 10, oy + 14, ox + 9,  oy + 18, 2, blue);
        stroke_line(layer, ox + 14, oy + 14, ox + 13, oy + 18, 2, blue);
        break;
    case ICON_SNOW:
        draw_cloud(layer, ox, oy, grey);
        fill_circle(layer, ox + 6,  oy + 16, 1, white);
        fill_circle(layer, ox + 10, oy + 18, 1, white);
        fill_circle(layer, ox + 14, oy + 16, 1, white);
        break;
    case ICON_THUNDER:
        draw_cloud(layer, ox, oy, dgrey);
        stroke_line(layer, ox + 11, oy + 12, ox + 8,  oy + 16, 2, yellow);
        stroke_line(layer, ox + 8,  oy + 16, ox + 12, oy + 16, 2, yellow);
        stroke_line(layer, ox + 12, oy + 16, ox + 10, oy + 19, 2, yellow);
        break;
    case ICON_UNKNOWN:
    default: {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = grey;
        d.border_opa = LV_OPA_COVER;
        d.border_width = 2;
        d.radius = LV_RADIUS_CIRCLE;
        lv_area_t ar = { ox + 4, oy + 4, ox + 16, oy + 16 };
        lv_draw_rect(layer, &d, &ar);
    } break;
    }
}

static void icon_draw_event_cb(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!obj || !layer) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int kind = (int)(intptr_t)lv_obj_get_user_data(obj);
    draw_icon(layer, &a, kind);
}

static lv_obj_t *make_icon_widget(lv_obj_t *parent, int kind)
{
    // Strip default styles (bg, border, padding, scrollbar) so the widget is a
    // transparent 20x20 canvas for the draw event to paint into.
    lv_obj_t *w = lv_obj_create(parent);
    lv_obj_remove_style_all(w);
    lv_obj_set_size(w, 20, 20);
    lv_obj_set_user_data(w, (void *)(intptr_t)kind);
    lv_obj_add_event_cb(w, icon_draw_event_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return w;
}

// --- weather code -> icon kind --------------------------------------------

static int wmo_to_icon(int code)
{
    switch (code) {
    case 0:  return ICON_SUN;
    case 1: case 2: return ICON_PARTLY;
    case 3:  return ICON_CLOUD;
    case 45: case 48: return ICON_FOG;
    case 51: case 53: case 55:
    case 56: case 57: return ICON_DRIZZLE;
    case 61: case 63: case 65:
    case 66: case 67:
    case 80: case 81: case 82: return ICON_RAIN;
    case 71: case 73: case 75: case 77:
    case 85: case 86: return ICON_SNOW;
    case 95: case 96: case 99: return ICON_THUNDER;
    }
    return ICON_UNKNOWN;
}

static int weekday_from_date(const std::string &date)
{
    if (date.size() < 10) return -1;
    int y = atoi(date.substr(0, 4).c_str());
    int m = atoi(date.substr(5, 2).c_str());
    int d = atoi(date.substr(8, 2).c_str());
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100;
    int J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7;  // 0=Sun ... 6=Sat
}

static const char *day_short(int iso_wday)
{
    static const char *names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (iso_wday < 0 || iso_wday > 6) return "---";
    return names[iso_wday];
}

// Cell = vertical stack (top label, icon, mid label, bot label) that shares a
// strip with its siblings via flex_grow. Cells (and their strips) are passive,
// not input-group members. When `is_current`, a thin accent outline on a
// tinted fill marks the hour/day that matches "now".
static lv_obj_t *make_cell(lv_obj_t *parent, const char *top, int icon_kind,
                           const char *mid, const char *bot, bool is_current)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 1, 0);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_radius(cell, 3, 0);
    if (is_current) {
        lv_obj_set_style_border_color(cell, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_bg_color(cell, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_20, 0);
    }

    auto add_label = [&](const char *t, lv_color_t color) {
        if (!t) return;
        lv_obj_t *l = lv_label_create(cell);
        lv_label_set_text(l, t);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_color(l, color, 0);
        lv_obj_set_style_text_font(l, get_weather_font(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    };

    add_label(top, UI_COLOR_MUTED);
    make_icon_widget(cell, icon_kind);
    add_label(mid, UI_COLOR_FG);
    add_label(bot, lv_color_hex(0x4AA3FF));
    return cell;
}
#endif // ARDUINO (forecast-grid renderer)

static void clear_all()
{
    if (hourly_col) lv_obj_clean(hourly_col);
    if (daily_col)  lv_obj_clean(daily_col);
}

#ifdef ARDUINO

static std::string json_str(cJSON *obj, const char *key)
{
    if (!obj) return "";
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || !v->valuestring) return "";
    return sanitize_ascii(v->valuestring);
}

struct GeoData {
    std::string location;
    double lat = 0, lon = 0;
};

static bool parse_geo(const std::string &body, GeoData &out, std::string &err)
{
    cJSON *root_json = cJSON_Parse(body.c_str());
    if (!root_json) { err = "geo parse"; return false; }

    std::string status = json_str(root_json, "status");
    if (status != "success") {
        cJSON_Delete(root_json);
        err = "geo status";
        return false;
    }

    std::string city = json_str(root_json, "city");
    std::string region = json_str(root_json, "regionName");
    std::string country = json_str(root_json, "country");
    out.location = city;
    if (!region.empty() && region != city) {
        if (!out.location.empty()) out.location += ", ";
        out.location += region;
    }
    if (out.location.empty()) out.location = country;

    cJSON *lat_j = cJSON_GetObjectItemCaseSensitive(root_json, "lat");
    cJSON *lon_j = cJSON_GetObjectItemCaseSensitive(root_json, "lon");
    if (cJSON_IsNumber(lat_j) && cJSON_IsNumber(lon_j)) {
        out.lat = lat_j->valuedouble;
        out.lon = lon_j->valuedouble;
    } else {
        cJSON_Delete(root_json);
        err = "geo coords";
        return false;
    }

    cJSON_Delete(root_json);
    return true;
}

// --- location cache -------------------------------------------------------
// Skipping the geo lookup on every fetch halves the total request count. IP
// geolocation is stable enough that caching across sessions is fine; we bump
// WEATHER_CACHE_VER if the stored schema ever changes.
//
// A cache is valid only for the source it was fetched for: the `key` field is
// "auto" for IP-based lookups or the user-supplied city name for manual mode.
// Switching modes invalidates the cache automatically.

static bool load_cached_geo(const std::string &want_key, GeoData &out)
{
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, true)) return false;
    int ver = p.getInt("ver", 0);
    if (ver != WEATHER_CACHE_VER) { p.end(); return false; }
    String key = p.getString("key", "");
    String loc = p.getString("loc", "");
    float lat = p.getFloat("lat", 0);
    float lon = p.getFloat("lon", 0);
    p.end();
    if (loc.length() == 0) return false;
    if (want_key != key.c_str()) return false;
    out.location = loc.c_str();
    out.lat = lat;
    out.lon = lon;
    return true;
}

static void save_cached_geo(const std::string &key, const GeoData &in)
{
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, false)) return;
    p.putInt("ver", WEATHER_CACHE_VER);
    p.putString("key", key.c_str());
    p.putString("loc", in.location.c_str());
    p.putFloat("lat", (float)in.lat);
    p.putFloat("lon", (float)in.lon);
    p.end();
}

static bool fetch_geo_ip(GeoData &out, std::string &err)
{
    std::string body;
    if (!hw_http_get_string(GEO_URL, body, &err)) return false;
    return parse_geo(body, out, err);
}

static bool fetch_geo_city(const std::string &city, GeoData &out, std::string &err)
{
    // Percent-encode the city (may contain UTF-8) for the query string.
    std::string enc = hal::url_encode(city);
    char direct_url[256];
    snprintf(direct_url, sizeof(direct_url),
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json",
        enc.c_str());
    std::string body;
    if (!hw_http_get_string(direct_url, body, &err)) return false;

    cJSON *j = cJSON_Parse(body.c_str());
    if (!j) { err = "geo parse"; return false; }

    cJSON *results = cJSON_GetObjectItemCaseSensitive(j, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(j);
        err = "city not found";
        return false;
    }
    cJSON *r0 = cJSON_GetArrayItem(results, 0);
    cJSON *lat_j = cJSON_GetObjectItemCaseSensitive(r0, "latitude");
    cJSON *lon_j = cJSON_GetObjectItemCaseSensitive(r0, "longitude");
    if (!cJSON_IsNumber(lat_j) || !cJSON_IsNumber(lon_j)) {
        cJSON_Delete(j);
        err = "geo coords";
        return false;
    }
    out.lat = lat_j->valuedouble;
    out.lon = lon_j->valuedouble;
    out.location = sanitize_ascii(city);
    cJSON_Delete(j);
    return true;
}

// Returns 0-23 if system time is synced, otherwise -1. Used to mark the
// current slot in the 24h strip.
static int current_local_hour()
{
    time_t now = time(nullptr);
    if (now < 1000000000) return -1;
    struct tm info;
    localtime_r(&now, &info);
    return info.tm_hour;
}

// --- forecast cache -------------------------------------------------------
// The forecast itself is cached in NVS as a compact binary blob. Open-meteo's
// 7-day response is ~5 KB raw and would blow past the NVS string limit, but
// the subset we actually render (8 hourly cells, 7 daily cells) packs into
// ~190 bytes — easy to persist and cheap to parse on open.
//
// The cache is keyed the same way as the geo cache (user_city or "auto"), so
// switching location automatically invalidates it.

struct CacheHour {
    uint8_t hr;      // 0-23
    uint8_t code;    // WMO weather code
    int8_t  temp;    // Celsius
    uint8_t pop;     // precipitation probability, 0-100
};

struct CacheDay {
    char    date[11];   // "YYYY-MM-DD" + NUL — consumed by weekday_from_date()
    uint8_t code;
    int8_t  hi;
    int8_t  lo;
    uint8_t pop;
};

struct ForecastCache {
    uint32_t  ts;           // unix time of fetch (0 = unknown clock)
    uint8_t   hourly_count;
    uint8_t   daily_count;
    CacheHour hourly[8];
    CacheDay  daily[7];
};

static bool load_forecast_cache(const std::string &want_key, ForecastCache &out)
{
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, true)) return false;
    int ver = p.getInt("fcv", 0);
    if (ver != WEATHER_CACHE_VER) { p.end(); return false; }
    String key = p.getString("fck", "");
    if (want_key != key.c_str()) { p.end(); return false; }
    memset(&out, 0, sizeof(out));
    size_t got = p.getBytes("fcd", &out, sizeof(out));
    p.end();
    return got == sizeof(out);
}

static void save_forecast_cache(const std::string &key, const ForecastCache &in)
{
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, false)) return;
    p.putInt("fcv", WEATHER_CACHE_VER);
    p.putString("fck", key.c_str());
    p.putBytes("fcd", &in, sizeof(in));
    p.end();
}

// --- JSON -> cache --------------------------------------------------------
// open-meteo's `hourly` starts at the current day's 00:00 local — we walk
// every 3rd entry from index 0 to land 8 points of 24h coverage.

static void build_hourly_cache(cJSON *hourly, ForecastCache &out)
{
    out.hourly_count = 0;
    if (!hourly) return;
    cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    cJSON *t_arr = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    cJSON *w_arr = cJSON_GetObjectItemCaseSensitive(hourly, "weathercode");
    cJSON *p_arr = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
    if (!cJSON_IsArray(time_arr)) return;
    int n = cJSON_GetArraySize(time_arr);
    for (int i = 0; i < n && i < 24 && out.hourly_count < 8; i += 3) {
        cJSON *t = cJSON_GetArrayItem(time_arr, i);
        cJSON *tmp = cJSON_GetArrayItem(t_arr, i);
        cJSON *wc = cJSON_GetArrayItem(w_arr, i);
        cJSON *pp = cJSON_IsArray(p_arr) ? cJSON_GetArrayItem(p_arr, i) : nullptr;
        std::string tstr = (cJSON_IsString(t) && t->valuestring) ? t->valuestring : "";
        CacheHour &h = out.hourly[out.hourly_count++];
        h.hr = (tstr.size() >= 13) ? (uint8_t)atoi(tstr.substr(11, 2).c_str()) : 0;
        int temp = cJSON_IsNumber(tmp) ? (int)tmp->valuedouble : 0;
        h.temp = (int8_t)(temp < -128 ? -128 : temp > 127 ? 127 : temp);
        h.code = cJSON_IsNumber(wc) ? (uint8_t)wc->valuedouble : 0;
        int pop = cJSON_IsNumber(pp) ? (int)pp->valuedouble : 0;
        h.pop = (uint8_t)(pop < 0 ? 0 : pop > 100 ? 100 : pop);
    }
}

static void build_daily_cache(cJSON *daily, ForecastCache &out)
{
    out.daily_count = 0;
    if (!daily) return;
    cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(daily, "time");
    cJSON *hi_arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    cJSON *lo_arr = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *w_arr = cJSON_GetObjectItemCaseSensitive(daily, "weathercode");
    cJSON *p_arr = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    if (!cJSON_IsArray(time_arr)) return;
    int n = cJSON_GetArraySize(time_arr);
    for (int i = 0; i < n && out.daily_count < 7; i++) {
        cJSON *t = cJSON_GetArrayItem(time_arr, i);
        cJSON *hi = cJSON_GetArrayItem(hi_arr, i);
        cJSON *lo = cJSON_GetArrayItem(lo_arr, i);
        cJSON *wc = cJSON_GetArrayItem(w_arr, i);
        cJSON *pp = cJSON_IsArray(p_arr) ? cJSON_GetArrayItem(p_arr, i) : nullptr;
        CacheDay &d = out.daily[out.daily_count++];
        memset(d.date, 0, sizeof(d.date));
        if (cJSON_IsString(t) && t->valuestring) {
            strncpy(d.date, t->valuestring, sizeof(d.date) - 1);
        }
        int hi_v = cJSON_IsNumber(hi) ? (int)hi->valuedouble : 0;
        int lo_v = cJSON_IsNumber(lo) ? (int)lo->valuedouble : 0;
        d.hi = (int8_t)(hi_v < -128 ? -128 : hi_v > 127 ? 127 : hi_v);
        d.lo = (int8_t)(lo_v < -128 ? -128 : lo_v > 127 ? 127 : lo_v);
        d.code = cJSON_IsNumber(wc) ? (uint8_t)wc->valuedouble : 0;
        int pop = cJSON_IsNumber(pp) ? (int)pp->valuedouble : 0;
        d.pop = (uint8_t)(pop < 0 ? 0 : pop > 100 ? 100 : pop);
    }
}

// --- cache -> UI ----------------------------------------------------------
// Both the NVS-loaded cache path and the post-fetch path feed through here,
// so the cold-start and fresh-fetch renders are pixel-identical.
static void render_cache(const ForecastCache &c)
{
    lv_obj_clean(hourly_col);
    lv_obj_clean(daily_col);

    int now_hr = current_local_hour();
    for (int i = 0; i < c.hourly_count; i++) {
        const CacheHour &h = c.hourly[i];
        char h_buf[8], t_buf[8], p_buf[8];
        snprintf(h_buf, sizeof(h_buf), "%02dh", (int)h.hr);
        snprintf(t_buf, sizeof(t_buf), "%dC", (int)h.temp);
        snprintf(p_buf, sizeof(p_buf), "%d%%", (int)h.pop);
        // Slot covers [hr, hr+3); mark the one containing "now".
        bool is_current = (now_hr >= 0 && now_hr >= (int)h.hr &&
                           now_hr < (int)h.hr + 3);
        make_cell(hourly_col, h_buf, wmo_to_icon(h.code), t_buf, p_buf, is_current);
    }
    for (int i = 0; i < c.daily_count; i++) {
        const CacheDay &d = c.daily[i];
        int wday = weekday_from_date(std::string(d.date));
        const char *name = (i == 0) ? "Tod" : day_short(wday);
        char tr_buf[12], p_buf[8];
        snprintf(tr_buf, sizeof(tr_buf), "%d/%d", (int)d.hi, (int)d.lo);
        snprintf(p_buf, sizeof(p_buf), "%d%%", (int)d.pop);
        make_cell(daily_col, name, wmo_to_icon(d.code), tr_buf, p_buf, i == 0);
    }
}

// Single open-meteo call covering hourly (24h sampled) + daily (7d). Populates
// `out` with the compact cache form used by render_cache().
static bool fetch_weather(double lat, double lon, ForecastCache &out,
                          std::string &err)
{
    // Buffer headroom matters: the full URL with all params is ~300 chars
    // before lat/lon expand, and a too-small buffer silently truncates.
    char qs[400];
    snprintf(qs, sizeof(qs),
        "latitude=%.3f&longitude=%.3f"
        "&hourly=temperature_2m,weathercode,precipitation_probability"
        "&daily=weathercode,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
        "&forecast_days=7&timezone=auto",
        lat, lon);

    char direct_url[512];
    snprintf(direct_url, sizeof(direct_url),
             "https://api.open-meteo.com/v1/forecast?%s", qs);
    std::string body;
    if (!hw_http_get_string(direct_url, body, &err)) return false;

    cJSON *j = cJSON_Parse(body.c_str());
    if (!j) { err = "forecast parse"; return false; }

    memset(&out, 0, sizeof(out));
    build_hourly_cache(cJSON_GetObjectItemCaseSensitive(j, "hourly"), out);
    build_daily_cache(cJSON_GetObjectItemCaseSensitive(j, "daily"), out);
    cJSON_Delete(j);

    // Only record a usable timestamp when the system clock is actually set —
    // otherwise freshness comparisons against a 1970 `ts` would treat the
    // cache as ancient and defeat the point.
    time_t now_t = time(nullptr);
    out.ts = (now_t > 1000000000) ? (uint32_t)now_t : 0;
    return true;
}

#endif // ARDUINO

#ifdef ARDUINO
// What the worker hands back to the UI thread.
struct WeatherFetchResult {
    bool          ok = false;          // `forecast` is valid → render it
    ForecastCache forecast;
    std::string   status;              // status-label text ("" clears it)
    lv_color_t    status_color;
};

// Background HTTP worker: geocode → forecast → save cache. Touches only its
// arguments and the NVS caches, never the LVGL widget tree; the result is
// heap-owned and freed by s_w_job after the UI thread renders it.
static WeatherFetchResult *weather_fetch(const std::string &key,
                                         const std::string &user_city)
{
    WeatherFetchResult *res = new WeatherFetchResult();
    res->status_color = lv_palette_main(LV_PALETTE_RED);

    std::string err;
    GeoData g;
    if (!load_cached_geo(key, g)) {
        bool ok = user_city.empty() ? fetch_geo_ip(g, err)
                                    : fetch_geo_city(user_city, g, err);
        if (!ok && user_city == WEATHER_DEFAULT_CITY) {
            // Offline first-run: fall back to the bundled coordinates so
            // the forecast call can still proceed.
            g.location    = WEATHER_DEFAULT_CITY;
            g.lat         = WEATHER_DEFAULT_LAT;
            g.lon         = WEATHER_DEFAULT_LON;
            ok            = true;
        }
        if (!ok) {
            res->status = "geo fail: " + (err.empty() ? std::string("err") : err);
            return res;
        }
        save_cached_geo(key, g);
    }

    if (!fetch_weather(g.lat, g.lon, res->forecast, err)) {
        res->status = "forecast: " + err;
        return res;
    }
    save_forecast_cache(key, res->forecast);
    res->ok = true;
    res->status_color = UI_COLOR_MUTED;
    return res;
}

// Drain callback (LVGL thread, instance mutex held by lv_timer_handler).
static void weather_fetch_done(WeatherFetchResult &r)
{
    if (r.ok) render_cache(r.forecast);
    set_status(r.status.c_str(), r.status_color);
}

static core::OneShotJob<WeatherFetchResult> s_w_job(weather_fetch_done);
#endif

static void do_fetch()
{
#ifdef ARDUINO
    std::string user_city = weather_get_user_city();
    std::string key = user_city.empty() ? std::string("auto") : user_city;

    // Load any cached forecast for the current key up front — we'll use it
    // to render immediately (fresh *or* stale) so the panel is never blank
    // while we figure out whether to hit the network.
    ForecastCache cache;
    bool have_cache = load_forecast_cache(key, cache);

    time_t now_t = time(nullptr);
    bool clock_ok = now_t > 1000000000;
    bool cache_fresh = have_cache && clock_ok && cache.ts > 0 &&
                       (uint32_t)now_t >= cache.ts &&
                       ((uint32_t)now_t - cache.ts) < WEATHER_FRESH_TTL_SEC;

    // Fresh cache hit → pure NVS path, no WiFi, no HTTPS, no parsing a 5 KB
    // response. This is the whole point of the cache.
    if (cache_fresh) {
        render_cache(cache);
        set_status("", UI_COLOR_MUTED);
        return;
    }

    bool wifi_on = hw_get_wifi_enable();
    bool wifi_up = wifi_on && hw_get_wifi_connected();

    if (!wifi_up) {
        // No network. If we have *any* cache, show it — stale weather is far
        // more useful than a blank panel. Otherwise fall back to the old
        // error-only behavior.
        if (have_cache) {
            render_cache(cache);
            set_status(wifi_on ? "Offline -- showing cached."
                               : "WiFi off -- showing cached.",
                       UI_COLOR_MUTED);
        } else {
            clear_all();
            set_status(wifi_on ? "WiFi not connected."
                               : "WiFi is off -- enable it in Settings.",
                       UI_COLOR_MUTED);
        }
        return;
    }

    // Have WiFi but the cache is missing or stale. Paint the stale cache
    // under an "Updating..." banner so the user sees something instantly
    // while the bg fetch runs.
    if (have_cache) {
        render_cache(cache);
        set_status("Updating...", UI_COLOR_ACCENT);
    } else {
        clear_all();
        set_status("Fetching...", UI_COLOR_ACCENT);
    }

    // A fetch still in flight (user re-tapped the panel or re-entered the
    // app) is re-attached rather than duplicated, so its result still lands.
    // 8 KB stack: Open-Meteo runs over HTTPS, and mbedtls TLS handshake
    // plus cert chain easily eats 6 KB on its own.
    auto started = s_w_job.start(
        [key, user_city]() { return weather_fetch(key, user_city); },
        8192, "wx_bg", 2);
    if (started == core::OneShotJob<WeatherFetchResult>::Start::Failed) {
        set_status("task spawn fail", lv_palette_main(LV_PALETTE_RED));
    }
#else
    clear_all();
    set_status("Not supported on emulator.", UI_COLOR_MUTED);
#endif
}

static void root_click_cb(lv_event_t *e)
{
    (void)e;
    hw_feedback();
    do_fetch();
}

static void ui_weather_enter(lv_obj_t *parent)
{
    ui_show_back_button(back_btn_cb);

    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 4, 0);
    lv_obj_set_style_pad_row(parent, 1, 0);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // IMPORTANT: do NOT set text_font on the parent — local styles survive the
    // app's onStop() because the app-panel is shared, and a Montserrat-10 leak
    // here has previously shrunk every label in the next-opened app.

    // Tap anywhere on the panel to refresh — no button means no wasted row.
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, root_click_cb, LV_EVENT_CLICKED, nullptr);

    // Section label: 24h + status (status sits on the same row, right-aligned)
    lv_obj_t *hdr_row = lv_obj_create(parent);
    lv_obj_remove_style_all(hdr_row);
    lv_obj_set_width(hdr_row, lv_pct(100));
    lv_obj_set_height(hdr_row, LV_SIZE_CONTENT);
    lv_obj_remove_flag(hdr_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hdr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *h_hdr = lv_label_create(hdr_row);
    lv_label_set_text(h_hdr, "24h");
    lv_obj_set_style_text_color(h_hdr, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(h_hdr, get_weather_font(), 0);

    status_label = lv_label_create(hdr_row);
    lv_label_set_text(status_label, "");
    lv_obj_set_flex_grow(status_label, 1);
    lv_obj_set_style_text_color(status_label, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_font(status_label, get_weather_font(), 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_RIGHT, 0);

    hourly_col = lv_obj_create(parent);
    lv_obj_remove_style_all(hourly_col);
    lv_obj_set_width(hourly_col, lv_pct(100));
    lv_obj_set_height(hourly_col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(hourly_col, 2, 0);
    lv_obj_set_style_pad_all(hourly_col, 2, 0);
    lv_obj_remove_flag(hourly_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hourly_col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hourly_col, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(hourly_col, LV_OBJ_FLAG_CLICKABLE);

    // Section label: Week
    lv_obj_t *d_hdr = lv_label_create(parent);
    lv_label_set_text(d_hdr, "Week");
    lv_obj_set_style_text_color(d_hdr, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(d_hdr, get_weather_font(), 0);

    daily_col = lv_obj_create(parent);
    lv_obj_remove_style_all(daily_col);
    lv_obj_set_width(daily_col, lv_pct(100));
    lv_obj_set_height(daily_col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(daily_col, 2, 0);
    lv_obj_set_style_pad_all(daily_col, 2, 0);
    lv_obj_remove_flag(daily_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(daily_col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(daily_col, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(daily_col, LV_OBJ_FLAG_CLICKABLE);

    do_fetch();
}

static void ui_weather_exit(lv_obj_t *parent)
{
    ui_hide_back_button();
    // Belt-and-suspenders: if any previous build leaked a local text_font
    // style onto the shared app-panel, clear it on exit so the next app opens
    // with the theme default font instead of Montserrat-10.
    if (parent) {
        lv_obj_remove_local_style_prop(parent, LV_STYLE_TEXT_FONT, 0);
    }
    hourly_col = nullptr;
    daily_col = nullptr;
    status_label = nullptr;

#ifdef ARDUINO
    // Stop draining the bg fetch, if any. A running worker is left to finish
    // on its own (it never touches the UI tree) and frees its own result; a
    // finished-but-undrained one is dropped so a re-entry doesn't render
    // stale data into a fresh panel.
    s_w_job.teardown();
#endif
}

class WeatherApp : public core::App {
public:
    WeatherApp() : core::App("Weather") {}
    void onStart(lv_obj_t *parent) override {
        setRoot(parent);
        ui_weather_enter(parent);
    }
    void onStop() override {
        ui_weather_exit(getRoot());
        core::App::onStop();
    }
};

} // namespace

namespace apps {
APP_FACTORY(make_weather_app, WeatherApp)
} // namespace apps

// Settings-facing helpers for the user-chosen weather city. Defined in this
// translation unit so the NVS layout stays co-located with the fetch logic.
//
// "user_city" semantics: key absent (first boot) = WEATHER_DEFAULT_CITY;
// stored "" = auto (IP geolocation); anything else = that city.
std::string weather_get_user_city()
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, true)) return std::string(WEATHER_DEFAULT_CITY);
    String c = p.getString("user_city", WEATHER_DEFAULT_CITY);
    p.end();
    return std::string(c.c_str());
#else
    return std::string(WEATHER_DEFAULT_CITY);
#endif
}

void weather_set_user_city(const char *city)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, false)) return;
    // Store "" explicitly for auto: removing the key would fall back to
    // WEATHER_DEFAULT_CITY on the next read and auto mode would never run.
    p.putString("user_city", (city && *city) ? city : "");
    // Drop the cached lat/lon *and* the cached forecast so the next fetch
    // resolves the new source from scratch — otherwise we'd render the old
    // city's forecast until the key-mismatch check in load_forecast_cache
    // eventually threw it out.
    p.remove("ver");
    p.remove("key");
    p.remove("loc");
    p.remove("lat");
    p.remove("lon");
    p.remove("fcv");
    p.remove("fck");
    p.remove("fcd");
    p.end();
#else
    (void)city;
#endif
}

// Save both the city label and its resolved coordinates so the first weather
// fetch can skip the geocoding round-trip. The cache key has to match what
// do_fetch() builds from user_city, otherwise it will refuse
// the cache and re-geocode.
void weather_set_user_location(const char *city, double lat, double lon)
{
#ifdef ARDUINO
    if (!city || !*city) {
        weather_set_user_city("");
        return;
    }
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, false)) return;
    p.putString("user_city", city);
    p.putInt("ver", WEATHER_CACHE_VER);
    p.putString("key", city);       // must match `key` built in do_fetch()
    p.putString("loc", city);       // displayed location label
    p.putFloat("lat", (float)lat);
    p.putFloat("lon", (float)lon);
    // The new location shares the NVS namespace with any leftover forecast
    // from the previous city; drop it so the next fetch starts clean rather
    // than briefly rendering the old city's forecast under the new label.
    p.remove("fcv");
    p.remove("fck");
    p.remove("fcd");
    p.end();
#else
    (void)city; (void)lat; (void)lon;
#endif
}

// Search the open-meteo geocoding API for cities matching `query`. Returns up
// to ~10 matches with display labels and coordinates ready to persist via
// weather_set_user_location(). Needs WiFi. weather_city_match lives in
// settings_internal.h.
bool weather_search_cities(const char *query,
                           std::vector<weather_city_match> &out,
                           std::string &err)
{
    out.clear();
#ifdef ARDUINO
    if (!query || !*query) { err = "empty query"; return false; }

    char url[384];
    snprintf(url, sizeof(url),
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=10&format=json",
        hal::url_encode(query).c_str());

    std::string body;
    if (!hw_http_get_string(url, body, &err)) return false;

    cJSON *j = cJSON_Parse(body.c_str());
    if (!j) { err = "search parse"; return false; }

    cJSON *results = cJSON_GetObjectItemCaseSensitive(j, "results");
    if (!cJSON_IsArray(results)) { cJSON_Delete(j); err = "no matches"; return false; }

    int n = cJSON_GetArraySize(results);
    for (int i = 0; i < n; ++i) {
        cJSON *r = cJSON_GetArrayItem(results, i);
        cJSON *nm  = cJSON_GetObjectItemCaseSensitive(r, "name");
        cJSON *a1  = cJSON_GetObjectItemCaseSensitive(r, "admin1");
        cJSON *cc  = cJSON_GetObjectItemCaseSensitive(r, "country");
        cJSON *lat = cJSON_GetObjectItemCaseSensitive(r, "latitude");
        cJSON *lon = cJSON_GetObjectItemCaseSensitive(r, "longitude");
        if (!cJSON_IsString(nm) || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) continue;

        weather_city_match m;
        m.name  = nm->valuestring;
        m.lat   = lat->valuedouble;
        m.lon   = lon->valuedouble;
        m.label = m.name;
        if (cJSON_IsString(a1) && a1->valuestring && *a1->valuestring &&
            strcmp(a1->valuestring, nm->valuestring) != 0) {
            m.label += ", ";
            m.label += a1->valuestring;
        }
        if (cJSON_IsString(cc) && cc->valuestring && *cc->valuestring) {
            m.label += ", ";
            m.label += cc->valuestring;
        }
        out.push_back(std::move(m));
    }
    cJSON_Delete(j);
    if (out.empty()) { err = "no matches"; return false; }
    return true;
#else
    (void)query;
    err = "emulator unsupported";
    return false;
#endif
}
