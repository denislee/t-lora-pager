/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/weather_payload.{h,cpp}.
 *
 * Covers the pure weather.push codec: parsing a hub payload into a
 * WeatherPayload (ForecastCache + optional GeoData), the per-field rejection
 * rules that never clamp, the round trip through weather_payload_encode(), and
 * the UTF-8 -> ASCII location sanitiser. A helper builds a full, valid payload
 * (the maximum 8 hourly / 7 daily cells plus a complete geo triple); the
 * rejection cases each perturb exactly one field of that shape.
 */
#include <unity.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "hal/weather_payload.h"
#include "hal/link/link_json.h"

void setUp(void) {}
void tearDown(void) {}

/* --- helpers ------------------------------------------------------------- */

/* One hourly / daily cell, each numeric token kept as the exact JSON text to
 * emit: a plain integer normally, "\"20\"" to force a string value. */
struct HourJson {
    std::string hr = "0";
    std::string code = "0";
    std::string temp = "0";
    std::string pop = "0";
};

struct DayJson {
    std::string date = "2026-01-01";
    std::string code = "0";
    std::string hi = "0";
    std::string lo = "0";
    std::string pop = "0";
};

/* Build the one-object wire payload. `lat_raw` / `lon_raw` of nullptr omit the
 * member entirely (to exercise partial-geo handling). */
static std::string build_payload(const std::vector<HourJson> &hours,
                                 const std::vector<DayJson> &days,
                                 bool with_key = true,
                                 bool with_city = true,
                                 const char *lat_raw = "-23.550000",
                                 const char *lon_raw = "-46.630000") {
    pagerlink::JsonWriter w;
    w.begin_object();

    if (with_key) w.key("key").str_val("Sao Paulo");
    w.key("ts").uint_val(1760000000u);

    w.key("hourly").begin_array();
    for (const HourJson &h : hours) {
        w.begin_object()
            .key("hr")
            .raw_val(h.hr.c_str())
            .key("code")
            .raw_val(h.code.c_str())
            .key("temp")
            .raw_val(h.temp.c_str())
            .key("pop")
            .raw_val(h.pop.c_str())
            .end_object();
    }
    w.end_array();

    w.key("daily").begin_array();
    for (const DayJson &d : days) {
        w.begin_object()
            .key("date")
            .str_val(d.date)
            .key("code")
            .raw_val(d.code.c_str())
            .key("hi")
            .raw_val(d.hi.c_str())
            .key("lo")
            .raw_val(d.lo.c_str())
            .key("pop")
            .raw_val(d.pop.c_str())
            .end_object();
    }
    w.end_array();

    if (with_city) w.key("city").str_val("S\xC3\xA3o Paulo");
    if (lat_raw) w.key("lat").raw_val(lat_raw);
    if (lon_raw) w.key("lon").raw_val(lon_raw);

    w.end_object();
    return w.str();
}

/* 8 hourly cells: hr 0,3,..21; code i; temp -5+i; pop 10*i. */
static std::vector<HourJson> full_hours(void) {
    std::vector<HourJson> v;
    for (int i = 0; i < 8; ++i) {
        HourJson h;
        h.hr = std::to_string(i * 3);
        h.code = std::to_string(i);
        h.temp = std::to_string(-5 + i);
        h.pop = std::to_string(i * 10);
        v.push_back(h);
    }
    return v;
}

/* 7 daily cells: 2026-01-01..07; code i; hi 30-i; lo 20-i; pop 10*i. */
static std::vector<DayJson> full_days(void) {
    std::vector<DayJson> v;
    for (int i = 0; i < 7; ++i) {
        DayJson d;
        d.date = "2026-01-0" + std::to_string(i + 1);
        d.code = std::to_string(i);
        d.hi = std::to_string(30 - i);
        d.lo = std::to_string(20 - i);
        d.pop = std::to_string(i * 10);
        v.push_back(d);
    }
    return v;
}

static std::string valid_payload(void) {
    return build_payload(full_hours(), full_days());
}

/* Parse must fail and name the offending field. */
static void assert_rejected(const std::string &json) {
    WeatherPayload p;
    std::string err;
    TEST_ASSERT_FALSE(weather_payload_parse(json, p, err));
    TEST_ASSERT_FALSE(err.empty());
}

/* --- tests --------------------------------------------------------------- */

static void test_valid_full_payload(void) {
    WeatherPayload p;
    std::string err = "unset";
    TEST_ASSERT_TRUE(weather_payload_parse(valid_payload(), p, err));
    TEST_ASSERT_TRUE(err.empty());

    TEST_ASSERT_EQUAL_STRING("Sao Paulo", p.key.c_str());
    TEST_ASSERT_EQUAL_UINT32(1760000000u, p.fc.ts);
    TEST_ASSERT_EQUAL_UINT8(8, p.fc.hourly_count);
    TEST_ASSERT_EQUAL_UINT8(7, p.fc.daily_count);

    TEST_ASSERT_EQUAL_UINT8(0, p.fc.hourly[0].hr);
    TEST_ASSERT_EQUAL_UINT8(0, p.fc.hourly[0].code);
    TEST_ASSERT_EQUAL_INT8(-5, p.fc.hourly[0].temp);
    TEST_ASSERT_EQUAL_UINT8(0, p.fc.hourly[0].pop);
    TEST_ASSERT_EQUAL_UINT8(21, p.fc.hourly[7].hr);
    TEST_ASSERT_EQUAL_INT8(2, p.fc.hourly[7].temp);
    TEST_ASSERT_EQUAL_UINT8(70, p.fc.hourly[7].pop);

    TEST_ASSERT_EQUAL_STRING("2026-01-01", p.fc.daily[0].date);
    TEST_ASSERT_EQUAL_INT8(30, p.fc.daily[0].hi);
    TEST_ASSERT_EQUAL_INT8(20, p.fc.daily[0].lo);
    TEST_ASSERT_EQUAL_STRING("2026-01-07", p.fc.daily[6].date);
    TEST_ASSERT_EQUAL_INT8(24, p.fc.daily[6].hi);
    TEST_ASSERT_EQUAL_UINT8(60, p.fc.daily[6].pop);

    TEST_ASSERT_TRUE(p.has_geo);
    TEST_ASSERT_EQUAL_STRING("Sao Paulo", p.geo.location.c_str());
    TEST_ASSERT_TRUE(std::fabs((-23.55) - (p.geo.lat)) < 1e-9);
    TEST_ASSERT_TRUE(std::fabs((-46.63) - (p.geo.lon)) < 1e-9);
}

static void test_round_trip(void) {
    WeatherPayload a;
    std::string err;
    TEST_ASSERT_TRUE(weather_payload_parse(valid_payload(), a, err));

    const std::string enc = weather_payload_encode(a.key, a.fc, &a.geo);

    WeatherPayload b;
    std::string err2 = "unset";
    TEST_ASSERT_TRUE(weather_payload_parse(enc, b, err2));
    TEST_ASSERT_TRUE(err2.empty());

    TEST_ASSERT_EQUAL_STRING(a.key.c_str(), b.key.c_str());
    TEST_ASSERT_EQUAL_UINT32(a.fc.ts, b.fc.ts);
    TEST_ASSERT_EQUAL_UINT8(a.fc.hourly_count, b.fc.hourly_count);
    TEST_ASSERT_EQUAL_UINT8(a.fc.daily_count, b.fc.daily_count);
    /* parse() memsets fc, so padding is zero on both sides too. */
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(&a.fc, &b.fc, sizeof(a.fc)));

    TEST_ASSERT_TRUE(b.has_geo);
    TEST_ASSERT_EQUAL_STRING(a.geo.location.c_str(), b.geo.location.c_str());
    TEST_ASSERT_TRUE(std::fabs((a.geo.lat) - (b.geo.lat)) < 1e-9);
    TEST_ASSERT_TRUE(std::fabs((a.geo.lon) - (b.geo.lon)) < 1e-9);
}

static void test_reject_nine_hourly(void) {
    std::vector<HourJson> h = full_hours();
    h.push_back(HourJson{});
    assert_rejected(build_payload(h, full_days()));
}

static void test_reject_hourly_temp_out_of_range(void) {
    std::vector<HourJson> h = full_hours();
    h[2].temp = "200";
    assert_rejected(build_payload(h, full_days()));
}

static void test_reject_hourly_pop_out_of_range(void) {
    std::vector<HourJson> h = full_hours();
    h[0].pop = "101";
    assert_rejected(build_payload(h, full_days()));
}

static void test_reject_hourly_hr_out_of_range(void) {
    std::vector<HourJson> h = full_hours();
    h[3].hr = "24";
    assert_rejected(build_payload(h, full_days()));
}

static void test_reject_bad_daily_date(void) {
    std::vector<DayJson> d = full_days();
    d[0].date = "2026-1-01";
    assert_rejected(build_payload(full_hours(), d));
}

static void test_reject_lat_out_of_range(void) {
    assert_rejected(build_payload(full_hours(), full_days(), true, true, "91", "-46.630000"));
}

static void test_reject_missing_key(void) {
    assert_rejected(build_payload(full_hours(), full_days(), false));
}

static void test_reject_temp_as_string(void) {
    std::vector<HourJson> h = full_hours();
    h[0].temp = "\"20\"";
    assert_rejected(build_payload(h, full_days()));
}

static void test_partial_geo_is_not_an_error(void) {
    /* city + lat, no lon: parse succeeds but the geo triple is incomplete. */
    WeatherPayload p;
    std::string err;
    const std::string json =
        build_payload(full_hours(), full_days(), true, true, "-23.550000", nullptr);
    TEST_ASSERT_TRUE(weather_payload_parse(json, p, err));
    TEST_ASSERT_FALSE(p.has_geo);
    TEST_ASSERT_TRUE(p.geo.location.empty());
}

static void test_sanitize_ascii(void) {
    TEST_ASSERT_EQUAL_STRING("Sao Paulo", weather_sanitize_ascii("S\xC3\xA3o Paulo").c_str());
}

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_full_payload);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_reject_nine_hourly);
    RUN_TEST(test_reject_hourly_temp_out_of_range);
    RUN_TEST(test_reject_hourly_pop_out_of_range);
    RUN_TEST(test_reject_hourly_hr_out_of_range);
    RUN_TEST(test_reject_bad_daily_date);
    RUN_TEST(test_reject_lat_out_of_range);
    RUN_TEST(test_reject_missing_key);
    RUN_TEST(test_reject_temp_as_string);
    RUN_TEST(test_partial_geo_is_not_an_error);
    RUN_TEST(test_sanitize_ascii);
    return UNITY_END();
}
