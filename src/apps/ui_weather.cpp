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
 * keeps forecasting for the old coordinates until the user picks a city (or
 * re-selects auto) in Settings » Weather.
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
 *   hooks:   weather_cache_set_change_hook(on_cache_changed)
 *                                       — set on enter, cleared on exit; the
 *                                          hook only posts to the LVGL thread
 *   job:     s_w_job (direct HTTPS / phone-step fetcher, core::OneShotJob;
 *            a thread on the emulator too)
 *                                       — teardown(): a running worker
 *                                          detached (its result freed on
 *                                          arrival unless a re-entry
 *                                          re-attaches it)
 * If you add new cached LVGL pointers or a background task, list them
 * above AND extend ui_weather_exit() — see the file header note about
 * panel-inherited styles; the same hazard applies to leaked timers.
 */
#include "../ui_define.h"
#include "../hal/wireless.h"
#include "../hal/system.h"
#include "../hal/str_encode.h"
#include "../hal/link.h"
#include "../hal/weather_cache.h"
#include "../hal/weather_link.h"
#include "../hal/weather_payload.h"
#include "../core/one_shot_job.h"
#include "../core/ui_post.h"
#include "app_registry.h"
#include "weather_test.h"
#include "settings_internal.h"  // weather_city_match + the weather_* decls
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Preferences.h>
extern "C" {
#include "cJSON.h"
}
#endif

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

// Cache generation last rendered. weather.push (and any other saver) bumps it;
// the cache change hook posts check_pushed_forecast to re-render (P7.47).
static uint32_t s_seen_gen = 0;

// --- async fetch state ----------------------------------------------------
//
// HTTP runs on a core::OneShotJob worker (s_w_job, below weather_fetch) so the
// LVGL thread keeps servicing input (keyboard / encoder / back button) during
// a fetch; the job delivers the result to the UI thread for rendering.

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    menu_show_deferred();
}

static void set_status(const char *text, lv_color_t color)
{
    if (!status_label) return;
    lv_label_set_text(status_label, text ? text : "");
    lv_obj_set_style_text_color(status_label, color, 0);
}

// Everything from here to make_cell() is the forecast-grid renderer, used by
// render_cache() on both builds (the emulator renders forecasts the phone
// pushes or returns over PagerLink).

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
    // The weather font only covers basic Latin: anything outside 0x20-0x7F
    // is stripped or downgraded, or it renders as tofu.
    return weather_sanitize_ascii(v->valuestring);
}

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

static bool fetch_geo_ip(GeoData &out, std::string &err)
{
    std::string body;
    if (!hw_http_get_string(GEO_URL, body, &err)) return false;
    return parse_geo(body, out, err);
}

// Percent-encode the query (may contain UTF-8), then ask the geocoding API
// for at most `count` matching cities. weather_city_match lives in
// settings_internal.h.
static bool geocode_search(const char *query, int count,
                           std::vector<weather_city_match> &out, std::string &err)
{
    out.clear();
    if (!query || !*query) { err = "empty query"; return false; }

    char url[384];
    snprintf(url, sizeof(url),
        "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&format=json",
        hal::url_encode(query).c_str(), count);

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
}

static bool fetch_geo_city(const std::string &city, GeoData &out, std::string &err)
{
    std::vector<weather_city_match> m;
    if (!geocode_search(city.c_str(), 1, m, err)) {
        if (err == "no matches") err = "city not found";
        return false;
    }
    out.lat = m[0].lat;
    out.lon = m[0].lon;
    out.location = weather_sanitize_ascii(city);
    return true;
}

#endif // ARDUINO

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

#ifdef ARDUINO

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

#endif // ARDUINO

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

#ifdef ARDUINO

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

// What the worker hands back to the UI thread.
struct WeatherFetchResult {
    bool          ok = false;          // `forecast` is valid → render it
    ForecastCache forecast;
    std::string   status;              // status-label text ("" clears it)
    lv_color_t    status_color;
};

// "phone: timeout: …" (chat_link-style link error) → "timeout: …" so the
// status reads "Phone: timeout: …" rather than repeating the prefix.
static std::string strip_phone_prefix(const std::string &e)
{
    static const char kPrefix[] = "phone: ";
    if (e.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) return e.substr(sizeof(kPrefix) - 1);
    return e;
}

// Worker side, phone step (HUB_REMOVAL_PLAN.md H2.3): weather.get for `key`.
// A reply for another key is refused; a good one is saved (forecast, plus the
// location when complete) and returned in `out`. Blocking.
static bool phone_fetch(const std::string &key, WeatherPayload &out, std::string &err)
{
    std::string lerr;
    HalResult<WeatherPayload> r = weather_link_get(key, lerr);
    if (!r) {
        err = strip_phone_prefix(lerr);
        return false;
    }
    if (r.value().key != key) {
        err = "reply for another location";
        return false;
    }
    out = r.value();
    weather_cache_save_forecast(key, out.fc);
    if (out.has_geo) weather_cache_save_geo(key, out.geo);
    return true;
}

#ifdef ARDUINO
// Worker side, direct step: geocode → forecast → save cache. On failure
// `status` holds the UI-ready error ("geo fail: …" / "forecast: …").
static bool direct_fetch(const std::string &key, const std::string &user_city,
                         ForecastCache &out, std::string &status)
{
    std::string err;
    GeoData g;
    if (!weather_cache_load_geo(key, g)) {
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
            status = "geo fail: " + (err.empty() ? std::string("err") : err);
            return false;
        }
        weather_cache_save_geo(key, g);
    }

    if (!fetch_weather(g.lat, g.lon, out, err)) {
        status = "forecast: " + err;
        return false;
    }
    weather_cache_save_forecast(key, out);
    return true;
}
#endif

// Background worker (D-H6, direct first): the direct HTTPS path when
// `try_direct` (hardware with Wi-Fi up); if that is unavailable or fails and
// the phone is connected, the phone step. Touches only its arguments and the
// caches, never the LVGL widget tree; the result is heap-owned and freed by
// the job after the UI thread renders it.
static WeatherFetchResult *weather_fetch(const std::string &key,
                                         const std::string &user_city,
                                         bool try_direct)
{
    WeatherFetchResult *res = new WeatherFetchResult();
    res->status_color = lv_palette_main(LV_PALETTE_RED);

    std::string direct_status;
#ifdef ARDUINO
    // With the phone up, Wi-Fi without internet would cost the 5 s connect
    // timeout (up to the 120 s TLS handshake default) before falling back to
    // it; a short TCP probe settles that first (P7.39). Without the phone
    // there is nothing to fall back to, so go straight to direct.
    if (try_direct &&
        (!hw_link_connected() ||
         hw_ping_internet("1.1.1.1", 53, 1500, nullptr, nullptr))) {
        if (direct_fetch(key, user_city, res->forecast, direct_status)) {
            res->ok = true;
            res->status_color = UI_COLOR_MUTED;
            return res;
        }
    }
#else
    (void)user_city;
    (void)try_direct;
#endif

    if (hw_link_connected()) {
        WeatherPayload p;
        std::string err;
        if (phone_fetch(key, p, err)) {
            res->ok = true;
            res->forecast = p.fc;
            res->status = "via phone";
            res->status_color = UI_COLOR_MUTED;
        } else {
            res->status = "Phone: " + err;
        }
        return res;
    }

    res->status = direct_status.empty() ? std::string("Connect Wi-Fi or your phone.")
                                        : direct_status;
    if (direct_status.empty()) res->status_color = UI_COLOR_MUTED;
    return res;
}

// Job delivery (LVGL thread).
static void weather_fetch_done(WeatherFetchResult &r)
{
    s_seen_gen = weather_cache_generation();  // our own save, already rendered
    if (r.ok) render_cache(r.forecast);
    set_status(r.status.c_str(), r.status_color);
}

// A thread on the emulator too, so the phone step (up to
// WEATHER_GET_TIMEOUT_MS) never blocks the LVGL thread there either.
static core::OneShotJob<WeatherFetchResult> s_w_job(weather_fetch_done);

// True while this app's own fetch may still save to the cache or deliver.
static bool fetch_in_flight()
{
    return s_w_job.busy();
}

// Chain (D-H6): fresh cache → render; else Wi-Fi up → direct (hardware);
// else / on direct failure, phone connected → weather.get; else the cache
// (stale) or a hint.
static void do_fetch()
{
    std::string user_city = weather_get_user_city();
    std::string key = weather_cache_key(user_city);

    // Load any cached forecast for the current key up front — we'll use it
    // to render immediately (fresh *or* stale) so the panel is never blank
    // while we figure out whether to hit the network.
    ForecastCache cache;
    bool have_cache = weather_cache_load_forecast(key, cache);

    time_t now_t = time(nullptr);
    bool clock_ok = now_t > 1000000000;
    bool cache_fresh = have_cache && clock_ok && cache.ts > 0 &&
                       (uint32_t)now_t >= cache.ts &&
                       ((uint32_t)now_t - cache.ts) < WEATHER_FRESH_TTL_SEC;

    // Fresh cache hit → pure cache path, no WiFi, no HTTPS, no parsing a 5 KB
    // response. This is the whole point of the cache.
    if (cache_fresh) {
        render_cache(cache);
        set_status("", UI_COLOR_MUTED);
        return;
    }

#ifdef ARDUINO
    bool wifi_on = hw_get_wifi_enable();
    bool wifi_up = wifi_on && hw_get_wifi_connected();
#else
    bool wifi_up = false;  // the direct path is hardware-only
#endif
    bool link_up = hw_link_connected();

    if (!wifi_up && !link_up) {
        // No network. If we have *any* cache, show it — stale weather is far
        // more useful than a blank panel.
        if (have_cache) {
            render_cache(cache);
#ifdef ARDUINO
            set_status(wifi_on ? "Offline -- showing cached."
                               : "WiFi off -- showing cached.",
                       UI_COLOR_MUTED);
#else
            set_status("Offline -- showing cached.", UI_COLOR_MUTED);
#endif
        } else {
            clear_all();
            set_status("Connect Wi-Fi or your phone.", UI_COLOR_MUTED);
        }
        return;
    }

    // Have a route but the cache is missing or stale. Paint the stale cache
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
        [key, user_city, wifi_up]() { return weather_fetch(key, user_city, wifi_up); },
        8192, "wx_bg", 2);
    if (started == core::OneShotJob<WeatherFetchResult>::Start::Failed) {
        set_status("task spawn fail", lv_palette_main(LV_PALETTE_RED));
    }
}

// LVGL thread: a forecast saved by someone else (weather.push) for the key we
// display is re-rendered from the cache.
static void check_pushed_forecast()
{
    if (!hourly_col || fetch_in_flight()) return;
    const uint32_t gen = weather_cache_generation();
    if (gen == s_seen_gen) return;
    s_seen_gen = gen;
    ForecastCache cache;
    if (!weather_cache_load_forecast(weather_cache_key(weather_get_user_city()), cache)) return;
    render_cache(cache);
    set_status("via phone", UI_COLOR_MUTED);
}

// Any task (weather.push runs on link_task): hop to the LVGL thread. A post
// that lands after exit finds hourly_col null and does nothing (P7.47).
static void on_cache_changed()
{
    core::ui_post(check_pushed_forecast);
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

    s_seen_gen = weather_cache_generation();
    do_fetch();
    weather_cache_set_change_hook(on_cache_changed);
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
    weather_cache_set_change_hook(nullptr);

    // Detach the bg fetch, if any. A running worker is left to finish on its
    // own (it never touches the UI tree); its result is freed on arrival
    // unless a re-entry re-attaches it.
    s_w_job.teardown();
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

#ifndef ARDUINO
WeatherTestResult weather_test_fetch(const std::string &key)
{
    WeatherTestResult t;
    WeatherPayload p;
    t.ok = phone_fetch(key, p, t.msg);
    if (t.ok) {
        t.city = p.has_geo ? p.geo.location : std::string();
        t.hourly_count = p.fc.hourly_count;
        t.daily_count = p.fc.daily_count;
    }
    return t;
}
#endif
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
    // Drop the cached lat/lon *and* the cached forecast: they belong to the
    // old source (weather_cache_load_geo/_forecast would refuse them on the
    // key check anyway), so the next fetch resolves the new one from scratch.
    // The HAL cache owns those keys — see weather_cache_clear().
    p.end();
    weather_cache_clear();
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
    weather_set_user_city(city);            // stores user_city, drops the old cache
    GeoData g;
    g.location = city;
    g.lat = lat;
    g.lon = lon;
    weather_cache_save_geo(weather_cache_key(city), g);   // key must match do_fetch()
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
    return geocode_search(query, 10, out, err);
#else
    (void)query;
    err = "emulator unsupported";
    return false;
#endif
}
