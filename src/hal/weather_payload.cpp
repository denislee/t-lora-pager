/**
 * @file      weather_payload.cpp
 * @brief     Implementation of weather_payload.h — pure, no I/O, no Arduino deps.
 *
 * Decodes/encodes the compact weather.push payload exchanged with the hub (one
 * JSON object, see weather_payload.h) and carries the ASCII sanitiser formerly
 * private to apps/ui_weather.cpp.
 */
#include "weather_payload.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "link/link_json.h"

std::string weather_sanitize_ascii(const std::string &in)
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

namespace {

/* "YYYY-MM-DD": exactly 10 bytes, digits everywhere except '-' at slots 4, 7. */
bool weather_date_valid(const std::string &d)
{
    if (d.size() != 10) return false;
    for (size_t i = 0; i < 10; ++i) {
        const char c = d[i];
        if (i == 4 || i == 7) {
            if (c != '-') return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

}  // namespace

bool weather_payload_parse(const std::string &json, WeatherPayload &out, std::string &err)
{
    pagerlink::JsonObject root;
    if (!root.parse(json)) {
        err = "not a JSON object";
        return false;
    }

    out.key.clear();
    std::memset(&out.fc, 0, sizeof(out.fc));
    out.geo = GeoData{};
    out.has_geo = false;

    /* key: required non-empty string of at most WEATHER_KEY_MAX bytes. */
    if (!root.get_string("key", out.key) || out.key.empty()) {
        err = "key missing or empty";
        return false;
    }
    if (out.key.size() > WEATHER_KEY_MAX) {
        err = "key too long";
        return false;
    }

    /* ts: required integer in [0, 4294967295]. */
    int64_t ts = 0;
    if (!root.get_int("ts", ts)) { err = "ts missing or not an integer"; return false; }
    if (ts < 0 || ts > 4294967295LL) { err = "ts out of range"; return false; }
    out.fc.ts = static_cast<uint32_t>(ts);

    /* hourly: required array of at most 8 integer objects. */
    std::string hourly_raw;
    if (!root.get_raw("hourly", hourly_raw)) { err = "hourly missing"; return false; }
    std::vector<std::string> hours;
    if (!pagerlink::json_array_items(hourly_raw, hours)) { err = "hourly not an array"; return false; }
    if (hours.size() > 8) { err = "hourly too many entries"; return false; }
    for (size_t i = 0; i < hours.size(); ++i) {
        const std::string idx = "hourly[" + std::to_string(i) + "]";
        pagerlink::JsonObject e;
        if (!e.parse(hours[i])) { err = idx + " not an object"; return false; }
        CacheHour &h = out.fc.hourly[i];
        int64_t v = 0;
        if (!e.get_int("hr", v)) { err = idx + ".hr missing or not an integer"; return false; }
        if (v < 0 || v > 23) { err = idx + ".hr out of range"; return false; }
        h.hr = static_cast<uint8_t>(v);
        if (!e.get_int("code", v)) { err = idx + ".code missing or not an integer"; return false; }
        if (v < 0 || v > 255) { err = idx + ".code out of range"; return false; }
        h.code = static_cast<uint8_t>(v);
        if (!e.get_int("temp", v)) { err = idx + ".temp missing or not an integer"; return false; }
        if (v < -128 || v > 127) { err = idx + ".temp out of range"; return false; }
        h.temp = static_cast<int8_t>(v);
        if (!e.get_int("pop", v)) { err = idx + ".pop missing or not an integer"; return false; }
        if (v < 0 || v > 100) { err = idx + ".pop out of range"; return false; }
        h.pop = static_cast<uint8_t>(v);
    }
    out.fc.hourly_count = static_cast<uint8_t>(hours.size());

    /* daily: required array of at most 7 objects with a YYYY-MM-DD date. */
    std::string daily_raw;
    if (!root.get_raw("daily", daily_raw)) { err = "daily missing"; return false; }
    std::vector<std::string> days;
    if (!pagerlink::json_array_items(daily_raw, days)) { err = "daily not an array"; return false; }
    if (days.size() > 7) { err = "daily too many entries"; return false; }
    for (size_t i = 0; i < days.size(); ++i) {
        const std::string idx = "daily[" + std::to_string(i) + "]";
        pagerlink::JsonObject e;
        if (!e.parse(days[i])) { err = idx + " not an object"; return false; }
        CacheDay &d = out.fc.daily[i];
        std::string date;
        if (!e.get_string("date", date)) { err = idx + ".date missing or not a string"; return false; }
        if (!weather_date_valid(date)) { err = idx + ".date invalid"; return false; }
        std::memset(d.date, 0, sizeof(d.date));
        std::memcpy(d.date, date.data(), 10);
        int64_t v = 0;
        if (!e.get_int("code", v)) { err = idx + ".code missing or not an integer"; return false; }
        if (v < 0 || v > 255) { err = idx + ".code out of range"; return false; }
        d.code = static_cast<uint8_t>(v);
        if (!e.get_int("hi", v)) { err = idx + ".hi missing or not an integer"; return false; }
        if (v < -128 || v > 127) { err = idx + ".hi out of range"; return false; }
        d.hi = static_cast<int8_t>(v);
        if (!e.get_int("lo", v)) { err = idx + ".lo missing or not an integer"; return false; }
        if (v < -128 || v > 127) { err = idx + ".lo out of range"; return false; }
        d.lo = static_cast<int8_t>(v);
        if (!e.get_int("pop", v)) { err = idx + ".pop missing or not an integer"; return false; }
        if (v < 0 || v > 100) { err = idx + ".pop out of range"; return false; }
        d.pop = static_cast<uint8_t>(v);
    }
    out.fc.daily_count = static_cast<uint8_t>(days.size());

    /* Optional geo: each present member must be valid, but only a complete
     * city + lat + lon triple is adopted (partial geo is not an error). */
    bool city_ok = false, lat_ok = false, lon_ok = false;
    GeoData geo;
    if (root.has("city")) {
        std::string city;
        if (!root.get_string("city", city)) { err = "city not a string"; return false; }
        geo.location = weather_sanitize_ascii(city);
        city_ok = true;
    }
    if (root.has("lat")) {
        double lat = 0;
        if (!root.get_double("lat", lat)) { err = "lat not a number"; return false; }
        if (!std::isfinite(lat) || lat < -90.0 || lat > 90.0) { err = "lat out of range"; return false; }
        geo.lat = lat;
        lat_ok = true;
    }
    if (root.has("lon")) {
        double lon = 0;
        if (!root.get_double("lon", lon)) { err = "lon not a number"; return false; }
        if (!std::isfinite(lon) || lon < -180.0 || lon > 180.0) { err = "lon out of range"; return false; }
        geo.lon = lon;
        lon_ok = true;
    }
    if (city_ok && lat_ok && lon_ok) {
        out.geo = geo;
        out.has_geo = true;
    }

    err.clear();
    return true;
}

std::string weather_payload_encode(const std::string &key, const ForecastCache &fc,
                                   const GeoData *geo)
{
    pagerlink::JsonWriter w;
    w.begin_object();

    w.key("key").str_val(key);
    w.key("ts").uint_val(fc.ts);

    w.key("hourly").begin_array();
    const uint8_t hc = (fc.hourly_count > 8) ? 8 : fc.hourly_count;
    for (uint8_t i = 0; i < hc; ++i) {
        const CacheHour &h = fc.hourly[i];
        w.begin_object()
            .key("hr")
            .int_val(h.hr)
            .key("code")
            .int_val(h.code)
            .key("temp")
            .int_val(h.temp)
            .key("pop")
            .int_val(h.pop)
            .end_object();
    }
    w.end_array();

    w.key("daily").begin_array();
    const uint8_t dc = (fc.daily_count > 7) ? 7 : fc.daily_count;
    for (uint8_t i = 0; i < dc; ++i) {
        const CacheDay &d = fc.daily[i];
        w.begin_object()
            .key("date")
            .str_val(std::string(d.date, strnlen(d.date, 10)))
            .key("code")
            .int_val(d.code)
            .key("hi")
            .int_val(d.hi)
            .key("lo")
            .int_val(d.lo)
            .key("pop")
            .int_val(d.pop)
            .end_object();
    }
    w.end_array();

    if (geo) {
        char lat[32], lon[32];
        std::snprintf(lat, sizeof(lat), "%.6f", geo->lat);
        std::snprintf(lon, sizeof(lon), "%.6f", geo->lon);
        w.key("city").str_val(geo->location);
        w.key("lat").raw_val(lat);
        w.key("lon").raw_val(lon);
    }

    w.end_object();
    return w.str();
}
