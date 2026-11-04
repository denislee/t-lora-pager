/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/nmea_rmc.{h,cpp} — the single-sentence RMC
 *            date+time gate used by the GPS clock sync.
 *
 * A sentence is only accepted when its checksum matches, it is an RMC with
 * status A, and its time and date are individually sane. Date and time come
 * from that one sentence, so a module's power-on default (1980-01-06, read as
 * 2080) or a stale date can never be paired with a live time.
 */
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hal/nmea_rmc.h"

namespace {

constexpr int64_t SENTINEL = -123456789;

const char *const GOOD_BODY =
    "GNRMC,120000.00,A,2332.0,S,04637.0,W,0.1,0.0,041126,,,A";

// Builds "$<body>*HH\r\n" with the XOR checksum of <body>. `corrupt` flips a
// checksum bit so the frame is otherwise well-formed but fails the gate.
size_t build_sentence(const char *body, char *out, bool corrupt)
{
    uint8_t sum = 0;
    size_t n = 0;
    out[n++] = '$';
    for (const char *p = body; *p != '\0'; ++p) {
        sum ^= (uint8_t)*p;
        out[n++] = *p;
    }
    if (corrupt) sum ^= 0x01;
    static const char HEX[] = "0123456789ABCDEF";
    out[n++] = '*';
    out[n++] = HEX[(sum >> 4) & 0x0F];
    out[n++] = HEX[sum & 0x0F];
    out[n++] = '\r';
    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

// Feeds `n` raw bytes one at a time. Returns true if any byte completed an
// acceptable sentence; *utc is updated by the clock only on acceptance.
bool feed_bytes(hal::NmeaRmcClock &clock, const char *bytes, size_t n,
                int64_t *utc)
{
    bool accepted = false;
    for (size_t i = 0; i < n; ++i) {
        if (clock.feed(bytes[i], utc)) accepted = true;
    }
    return accepted;
}

bool feed_raw(hal::NmeaRmcClock &clock, const char *bytes, int64_t *utc)
{
    return feed_bytes(clock, bytes, strlen(bytes), utc);
}

bool feed_sentence(hal::NmeaRmcClock &clock, const char *body, int64_t *utc)
{
    char frame[256];
    const size_t n = build_sentence(body, frame, false);
    return feed_bytes(clock, frame, n, utc);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_accepts_gnrmc_with_fraction(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_TRUE(feed_sentence(clock, GOOD_BODY, &utc));
    TEST_ASSERT_EQUAL_INT64(1793793600, utc);
}

static void test_accepts_gp_talker_without_fraction(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_TRUE(feed_sentence(clock, "GPRMC,120000,A,,,,,,,041126,,", &utc));
    TEST_ASSERT_EQUAL_INT64(1793793600, utc);
}

static void test_accepts_leap_day(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_TRUE(feed_sentence(clock, "GNRMC,235959.00,A,,,,,,,290228,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(1835481599, utc);
}

static void test_accepts_leap_second(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_TRUE(feed_sentence(clock, "GNRMC,235960.00,A,,,,,,,311226,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(1798761600, utc);
}

static void test_rejects_status_v(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,120000.00,V,,,,,,,041126,,,N", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_1980_default_status_v(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,235943.00,V,,,,,,,060180,,,N", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_1980_default_status_a(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,235943.00,A,,,,,,,060180,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_empty_rmc(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,,V,,,,,,,,,,N", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_status_a_with_empty_date(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,120000.00,A,,,,,,,", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_status_a_with_empty_time(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,,A,,,,,,,041126", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_year_2023(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,120000.00,A,,,,,,,041123,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_month_13(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,120000.00,A,,,,,,,041326,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_day_zero(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,120000.00,A,,,,,,,001126,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_hour_24(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,240000.00,A,,,,,,,041126,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_gga(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock,
        "GNGGA,120000.00,2332.0,S,04637.0,W,1,08,1.0,700.0,M,0.0,M,,", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_bad_checksum(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    char frame[256];
    const size_t n = build_sentence(GOOD_BODY, frame, true);
    TEST_ASSERT_FALSE(feed_bytes(clock, frame, n, &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_rejects_body_without_checksum(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    char frame[256];
    snprintf(frame, sizeof(frame), "$%s\r\n", GOOD_BODY);
    TEST_ASSERT_FALSE(feed_raw(clock, frame, &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

static void test_dollar_restarts_mid_sentence(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    // A truncated sentence is abandoned by the '$' of the next one, and the
    // good sentence is then accepted exactly once.
    TEST_ASSERT_FALSE(feed_raw(clock, "$GNRMC,120000.00,A,2332.0,S", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
    TEST_ASSERT_TRUE(feed_sentence(clock, GOOD_BODY, &utc));
    TEST_ASSERT_EQUAL_INT64(1793793600, utc);
}

static void test_overlong_junk_then_good_sentence(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    char junk[201];
    memset(junk, 'x', sizeof(junk) - 1);
    junk[sizeof(junk) - 1] = '\0';

    clock.feed('$', &utc);
    TEST_ASSERT_FALSE(feed_raw(clock, junk, &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
    TEST_ASSERT_TRUE(feed_sentence(clock, GOOD_BODY, &utc));
    TEST_ASSERT_EQUAL_INT64(1793793600, utc);
}

static void test_reset_after_empty_rmc_then_good(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,,V,,,,,,,,,,N", &utc));
    clock.reset();
    TEST_ASSERT_TRUE(feed_sentence(clock, GOOD_BODY, &utc));
    TEST_ASSERT_EQUAL_INT64(1793793600, utc);
}

static void test_utc_untouched_on_rejection(void)
{
    hal::NmeaRmcClock clock;
    int64_t utc = SENTINEL;
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,120000.00,V,,,,,,,041126,,,N", &utc));
    TEST_ASSERT_FALSE(feed_sentence(clock, "GNRMC,235943.00,A,,,,,,,060180,,,A", &utc));
    TEST_ASSERT_EQUAL_INT64(SENTINEL, utc);
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_accepts_gnrmc_with_fraction);
    RUN_TEST(test_accepts_gp_talker_without_fraction);
    RUN_TEST(test_accepts_leap_day);
    RUN_TEST(test_accepts_leap_second);
    RUN_TEST(test_rejects_status_v);
    RUN_TEST(test_rejects_1980_default_status_v);
    RUN_TEST(test_rejects_1980_default_status_a);
    RUN_TEST(test_rejects_empty_rmc);
    RUN_TEST(test_rejects_status_a_with_empty_date);
    RUN_TEST(test_rejects_status_a_with_empty_time);
    RUN_TEST(test_rejects_year_2023);
    RUN_TEST(test_rejects_month_13);
    RUN_TEST(test_rejects_day_zero);
    RUN_TEST(test_rejects_hour_24);
    RUN_TEST(test_rejects_gga);
    RUN_TEST(test_rejects_bad_checksum);
    RUN_TEST(test_rejects_body_without_checksum);
    RUN_TEST(test_dollar_restarts_mid_sentence);
    RUN_TEST(test_overlong_junk_then_good_sentence);
    RUN_TEST(test_reset_after_empty_rmc_then_good);
    RUN_TEST(test_utc_untouched_on_rejection);
    return UNITY_END();
}
