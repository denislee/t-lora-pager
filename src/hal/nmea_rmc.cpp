/**
 * @file      nmea_rmc.cpp
 * @brief     Single-sentence NMEA RMC date+time gate. See nmea_rmc.h.
 */
#include "nmea_rmc.h"

namespace {

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant's
// days_from_civil). ESP32 newlib ships no timegm(); this replaces the old
// TZ=UTC0 + mktime() round trip, which flipped the process-global TZ under
// every other task that calls localtime() (DC2.0.3).
int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                        // [0, 399]
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool all_digits(const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (!is_digit(s[i])) return false;
    }
    return true;
}

int digits_to_int(const char *s, size_t n)
{
    int v = 0;
    for (size_t i = 0; i < n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

struct Field {
    const char *ptr;
    size_t len;
};

// Splits a comma-separated NMEA payload. Returns the total field count, which
// may exceed `max`; only the first `max` fields are stored.
size_t split_fields(const char *s, size_t n, Field *out, size_t max)
{
    size_t count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i == n || s[i] == ',') {
            if (count < max) {
                out[count].ptr = s + start;
                out[count].len = i - start;
            }
            ++count;
            start = i + 1;
        }
    }
    return count;
}

// Validates one buffered sentence (text after '$', including "*HH") and, if
// acceptable, produces its UTC instant.
bool accept_sentence(const char *buf, size_t len, int64_t *utc)
{
    if (len < 3) return false;

    // The '*' must sit two hex digits before the end of the buffer.
    const size_t star = len - 3;
    if (buf[star] != '*') return false;
    const int hi = hex_digit(buf[star + 1]);
    const int lo = hex_digit(buf[star + 2]);
    if (hi < 0 || lo < 0) return false;

    unsigned char sum = 0;
    for (size_t i = 0; i < star; ++i) sum ^= (unsigned char)buf[i];
    if (sum != (unsigned char)((hi << 4) | lo)) return false;

    Field f[16];
    if (split_fields(buf, star, f, 16) < 10) return false;

    // Address: any talker, exactly five chars ending in RMC.
    if (f[0].len != 5 || f[0].ptr[2] != 'R' || f[0].ptr[3] != 'M'
            || f[0].ptr[4] != 'C') {
        return false;
    }

    // Time: hhmmss with at least six chars; a fraction may follow the dot.
    if (f[1].len < 6 || !all_digits(f[1].ptr, 6)) return false;
    if (f[1].len > 6 && f[1].ptr[6] != '.') return false;
    const int hour = digits_to_int(f[1].ptr, 2);
    const int minute = digits_to_int(f[1].ptr + 2, 2);
    const int second = digits_to_int(f[1].ptr + 4, 2);
    if (hour > 23 || minute > 59 || second > 60) return false;

    // Status A: a real fix. V is the module's no-fix/default report.
    if (f[2].len != 1 || f[2].ptr[0] != 'A') return false;

    // Date: ddmmyy.
    if (f[9].len != 6 || !all_digits(f[9].ptr, 6)) return false;
    const int day = digits_to_int(f[9].ptr, 2);
    const int month = digits_to_int(f[9].ptr + 2, 2);
    const int year = 2000 + digits_to_int(f[9].ptr + 4, 2);
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    // 1980-01-06 (yy=80) is the modules' power-on default and would read as
    // 2080; 2023 is below anything a live fix reports.
    if (year < 2024 || year > 2079) return false;

    *utc = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400
           + hour * 3600 + minute * 60 + second;
    return true;
}

} // namespace

namespace hal {

void NmeaRmcClock::reset()
{
    len_ = 0;
    in_sentence_ = false;
}

bool NmeaRmcClock::feed(char c, int64_t *utc)
{
    if (c == '$') {
        in_sentence_ = true;
        len_ = 0;
        return false;
    }
    if (!in_sentence_) return false;

    if (c == '\r' || c == '\n') {
        in_sentence_ = false;
        int64_t value = 0;
        if (!accept_sentence(buf_, len_, &value)) return false;
        if (utc) *utc = value;
        return true;
    }

    if (len_ >= sizeof(buf_)) {
        // Overlong sentence: abandon it until the next '$'.
        in_sentence_ = false;
        len_ = 0;
        return false;
    }

    buf_[len_++] = c;
    return false;
}

} // namespace hal
