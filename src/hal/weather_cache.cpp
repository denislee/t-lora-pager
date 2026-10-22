/**
 * @file      weather_cache.cpp
 * @brief     weather_cache_* — see weather_cache.h.
 */
#include "weather_cache.h"

#include <atomic>
#include <cstring>

#ifdef ARDUINO
#include <Preferences.h>
#else
#include <mutex>
#endif

static std::atomic<uint32_t> g_generation{0};
static std::atomic<void (*)()> g_change_hook{nullptr};

// Bump the generation and tell the one registered observer, if any. Runs on
// the saving task with no store lock held, so the hook may read the cache.
static void bump_generation()
{
    g_generation.fetch_add(1);
    if (void (*hook)() = g_change_hook.load()) hook();
}

void weather_cache_set_change_hook(void (*hook)())
{
    g_change_hook.store(hook);
}

uint32_t weather_cache_generation()
{
    return g_generation.load();
}

std::string weather_cache_key(const std::string &user_city)
{
    return user_city.empty() ? std::string("auto") : user_city;
}

#ifdef ARDUINO

bool weather_cache_load_geo(const std::string &want_key, GeoData &out)
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

void weather_cache_save_geo(const std::string &key, const GeoData &in)
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

bool weather_cache_load_forecast(const std::string &want_key, ForecastCache &out)
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

void weather_cache_save_forecast(const std::string &key, const ForecastCache &in)
{
    Preferences p;
    if (!p.begin(WEATHER_PREFS_NS, false)) return;
    p.putInt("fcv", WEATHER_CACHE_VER);
    p.putString("fck", key.c_str());
    p.putBytes("fcd", &in, sizeof(in));
    p.end();
    bump_generation();
}

#else  // !ARDUINO — process-local store (emulator, native tests)

namespace {

std::mutex g_mu;
bool g_geo_set = false;
std::string g_geo_key;
GeoData g_geo;
bool g_fc_set = false;
std::string g_fc_key;
ForecastCache g_fc;

}  // namespace

bool weather_cache_load_geo(const std::string &want_key, GeoData &out)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_geo_set || g_geo.location.empty() || g_geo_key != want_key) return false;
    out = g_geo;
    return true;
}

void weather_cache_save_geo(const std::string &key, const GeoData &in)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_geo_key = key;
    g_geo = in;
    // Same precision NVS keeps (putFloat).
    g_geo.lat = (float)in.lat;
    g_geo.lon = (float)in.lon;
    g_geo_set = true;
}

bool weather_cache_load_forecast(const std::string &want_key, ForecastCache &out)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_fc_set || g_fc_key != want_key) return false;
    memcpy(&out, &g_fc, sizeof(out));
    return true;
}

void weather_cache_save_forecast(const std::string &key, const ForecastCache &in)
{
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_fc_key = key;
        memcpy(&g_fc, &in, sizeof(g_fc));
        g_fc_set = true;
    }
    // Outside g_mu: the hook runs on this task and may call straight back
    // into the cache (the Weather app's re-render reads it).
    bump_generation();
}

#endif
