/**
 * @file      settings_weather.cpp
 * @brief     Settings » Weather subpage. Extracted from ui_settings.cpp; see
 *            settings_internal.h for the cross-TU contract.
 */
#include "../ui_define.h"
#include "../ui_list_picker.h"
#include "../hal/link/link_json.h"
#include "../hal/str_encode.h"
#include "../core/one_shot_job.h"
#include "settings_internal.h"

namespace weather_cfg {

static lv_obj_t *g_city_label = nullptr;
static lv_obj_t *g_status_label = nullptr;
static std::vector<weather_city_match> g_search_results;

static void refresh_label()
{
    if (!g_city_label) return;
    std::string city = weather_get_user_city();
    lv_label_set_text_fmt(g_city_label, "City: %s",
                          city.empty() ? "(auto from IP)" : city.c_str());
}

static void set_status(const char *text, lv_color_t color)
{
    if (!g_status_label) return;
    lv_label_set_text(g_status_label, text ? text : "");
    lv_obj_set_style_text_color(g_status_label, color, 0);
}

// User picked one of the geocoding results. Persist the exact name + lat/lon
// through the settings registry so the Weather app's first fetch skips the
// geocoding round-trip entirely.
static void city_picked_cb(int index, void *ud)
{
    (void)ud;
    if (index < 0 || (size_t)index >= g_search_results.size()) {
        set_status("Cancelled", UI_COLOR_MUTED);
        return;
    }
    const weather_city_match &m = g_search_results[(size_t)index];
    char lat[24], lon[24];
    snprintf(lat, sizeof(lat), "%.5f", m.lat);
    snprintf(lon, sizeof(lon), "%.5f", m.lon);
    pagerlink::JsonWriter w;
    w.begin_object()
        .key("name").str_val(m.name)
        .key("lat").raw_val(lat)
        .key("lon").raw_val(lon)
        .end_object();
    settings_commit("weather.location", settings::Value::of_json(w.str()));
    refresh_label();
    set_status("Saved", lv_palette_main(LV_PALETTE_GREEN));
    g_search_results.clear();
}

// Open the city picker over the current g_search_results (which city_picked_cb
// indexes into).
static void open_city_picker()
{
    std::vector<std::string> labels;
    labels.reserve(g_search_results.size());
    for (const auto &m : g_search_results) labels.push_back(m.label);
    set_status("", UI_COLOR_MUTED);
    ui_list_picker_open("Pick a city", labels, city_picked_cb, nullptr);
}

// §2.17: the geocoding lookup is a blocking HTTPS call; running it inline froze
// the LVGL thread (dead back button) for the request duration. Run it on a
// core::OneShotJob worker and deliver the result on the LVGL thread — same
// lifecycle contract as the Telegram-favorites fetch: the worker touches only
// its own heap result (never the widget tree), and reset_state() detaches it
// before the subpage widgets are freed.
struct CitySearchResult {
    std::vector<weather_city_match> matches;
    bool ok = false;
    std::string err;
};

static void city_search_done(CitySearchResult &res)
{
    if (!res.ok) {
        set_status(("Search failed: " + (res.err.empty() ? std::string("err") : res.err)).c_str(),
                   lv_palette_main(LV_PALETTE_RED));
        return;
    }
    g_search_results = std::move(res.matches);   // city_picked_cb indexes into this
    open_city_picker();
}

static core::OneShotJob<CitySearchResult> g_search_job(city_search_done);

void reset_state()
{
    g_city_label = nullptr;
    g_status_label = nullptr;
    g_search_results.clear();
    g_search_job.teardown();
}

// Text-prompt OK: query the open-meteo geocoding API for matches, then open
// a modal list of the server-accepted names. We never store a city that the
// API didn't return, so the forecast fetch can't fail on a typoed name.
static void search_entered_cb(const char *text, void *ud)
{
    (void)ud;
    if (!text) { set_status("Cancelled", UI_COLOR_MUTED); return; }
    std::string q = hal::trim(text);
    if (q.empty()) {
        // Empty submission clears the override and returns to auto.
        settings_commit("weather.location",
                        settings::Value::of_json("{\"auto\":true}"));
        refresh_label();
        set_status("Using auto (IP)", UI_COLOR_MUTED);
        return;
    }

    set_status("Searching...", UI_COLOR_ACCENT);
    // Geocode off-thread so the back button stays live; city_search_done
    // opens the picker (or shows the error) once the worker finishes.
    using Job = core::OneShotJob<CitySearchResult>;
    Job::Start started = g_search_job.start([q]() {
        CitySearchResult *res = new CitySearchResult();
        res->ok = weather_search_cities(q.c_str(), res->matches, res->err);
        return res;
    }, 8192, "wx_city", 2);
    if (started == Job::Start::Attached) {
        // A prior search is still in flight (e.g. the page was exited and
        // re-entered mid-fetch); its result still lands, and its picker
        // reflects the earlier query.
        set_status("Still searching...", UI_COLOR_ACCENT);
    } else if (started == Job::Start::Failed) {
        set_status("task spawn fail", lv_palette_main(LV_PALETTE_RED));
    }
}

static void btn_set_city_cb(lv_event_t *e)
{
    (void)e;
    std::string current = weather_get_user_city();
    ui_text_prompt("Weather city",
                   "Search for a city (empty = auto).",
                   current.c_str(), search_entered_cb, nullptr);
}

static void btn_clear_city_cb(lv_event_t *e)
{
    (void)e;
    settings_commit("weather.location", settings::Value::of_json("{\"auto\":true}"));
    refresh_label();
    set_status("Using auto (IP)", UI_COLOR_MUTED);
}

void build_subpage(lv_obj_t *menu, lv_obj_t *sub_page)
{
    (void)menu;
    lv_obj_set_style_pad_row(sub_page, 4, 0);

    lv_obj_t *status = lv_menu_cont_create(sub_page);
    g_city_label = lv_label_create(status);
    lv_obj_set_style_text_color(g_city_label, UI_COLOR_MUTED, 0);
    refresh_label();

    settings_button_row(sub_page, LV_SYMBOL_KEYBOARD, "Set city", btn_set_city_cb);

    settings_button_row(sub_page, LV_SYMBOL_REFRESH, "Use auto (IP)", btn_clear_city_cb);

    // Status line for search feedback ("Searching...", errors, "Saved").
    g_status_label = settings_muted_label(sub_page);
    lv_label_set_text(g_status_label, "");
}

} // namespace weather_cfg
