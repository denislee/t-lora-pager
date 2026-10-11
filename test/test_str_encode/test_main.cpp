/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/str_encode.{h,cpp} — the pure-logic string
 *            encoders shared by every HTTP/JSON body builder (chat, notes
 *            sync, weather, hub).
 *
 * Covers json_escape, url_encode, trim, parse_url and utf8_byte_pos.
 * hal::base64_encode is #ifdef
 * ARDUINO-gated (implemented via mbedtls, which isn't linked into
 * native_test) and every call site is itself under #ifdef ARDUINO, so it has
 * no portable pure-logic form to test here — left untested by construction,
 * not an oversight.
 */
#include <unity.h>

#include "hal/str_encode.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- json_escape ---- */

static void test_json_escape_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", hal::json_escape("").c_str());
}

static void test_json_escape_plain_passthrough(void)
{
    TEST_ASSERT_EQUAL_STRING("hello world", hal::json_escape("hello world").c_str());
}

static void test_json_escape_quote_and_backslash(void)
{
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c", hal::json_escape("a\"b\\c").c_str());
}

static void test_json_escape_named_control_chars(void)
{
    TEST_ASSERT_EQUAL_STRING("\\n\\r\\t", hal::json_escape("\n\r\t").c_str());
}

static void test_json_escape_other_control_char_as_u_escape(void)
{
    // 0x01 has no named escape, so it falls through to \u00XX.
    TEST_ASSERT_EQUAL_STRING("\\u0001", hal::json_escape("\x01").c_str());
}

static void test_json_escape_utf8_bytes_pass_through(void)
{
    // UTF-8 continuation bytes are >= 0x20 as unsigned char, so they're not
    // control-escaped — only the JSON-forbidden ASCII characters are.
    const char *euro = "\xE2\x82\xAC"; // U+20AC in UTF-8
    TEST_ASSERT_EQUAL_STRING(euro, hal::json_escape(euro).c_str());
}

/* ---- url_encode ---- */

static void test_url_encode_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", hal::url_encode("").c_str());
}

static void test_url_encode_unreserved_passthrough(void)
{
    const char *unreserved = "AZaz09-_.~";
    TEST_ASSERT_EQUAL_STRING(unreserved, hal::url_encode(unreserved).c_str());
}

static void test_url_encode_space_and_reserved(void)
{
    TEST_ASSERT_EQUAL_STRING("a%20b%2Fc%3Fd", hal::url_encode("a b/c?d").c_str());
}

static void test_url_encode_utf8_bytes_percent_encoded(void)
{
    // U+20AC (Euro sign) is E2 82 AC in UTF-8 — every byte gets %XX-encoded.
    TEST_ASSERT_EQUAL_STRING("%E2%82%AC", hal::url_encode("\xE2\x82\xAC").c_str());
}

static void test_url_encode_hex_digits_are_uppercase(void)
{
    // Byte 0xAB is not unreserved, so it must round-trip as "%AB", not "%ab".
    TEST_ASSERT_EQUAL_STRING("%AB", hal::url_encode("\xAB").c_str());
}

/* ---- trim ---- */

static void test_trim_empty_and_all_whitespace(void)
{
    TEST_ASSERT_EQUAL_STRING("", hal::trim("").c_str());
    TEST_ASSERT_EQUAL_STRING("", hal::trim(" \t\r\n ").c_str());
}

static void test_trim_both_ends(void)
{
    TEST_ASSERT_EQUAL_STRING("tok en", hal::trim("\t tok en\r\n").c_str());
}

static void test_trim_no_whitespace_passthrough(void)
{
    TEST_ASSERT_EQUAL_STRING("abc", hal::trim("abc").c_str());
}

/* ---- parse_url ---- */

static void test_parse_url_full(void)
{
    hal::UrlParts u;
    TEST_ASSERT_TRUE(hal::parse_url("http://10.0.0.2:8080/api/chat", u));
    TEST_ASSERT_EQUAL_STRING("http", u.scheme.c_str());
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", u.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(8080, u.port);
    TEST_ASSERT_EQUAL_STRING("/api/chat", u.path.c_str());
}

static void test_parse_url_scheme_default_ports(void)
{
    hal::UrlParts u;
    TEST_ASSERT_TRUE(hal::parse_url("http://hub.lan", u));
    TEST_ASSERT_EQUAL_UINT16(80, u.port);
    TEST_ASSERT_EQUAL_STRING("/", u.path.c_str());
    TEST_ASSERT_TRUE(hal::parse_url("https://hub.lan/x", u));
    TEST_ASSERT_EQUAL_UINT16(443, u.port);
}

static void test_parse_url_no_scheme(void)
{
    hal::UrlParts u;
    TEST_ASSERT_TRUE(hal::parse_url("hub.lan:9000", u));
    TEST_ASSERT_EQUAL_STRING("", u.scheme.c_str());
    TEST_ASSERT_EQUAL_STRING("hub.lan", u.host.c_str());
    TEST_ASSERT_EQUAL_UINT16(9000, u.port);
}

static void test_parse_url_bad_port_keeps_default(void)
{
    hal::UrlParts u;
    TEST_ASSERT_TRUE(hal::parse_url("http://h:0/", u));
    TEST_ASSERT_EQUAL_UINT16(80, u.port);
    TEST_ASSERT_TRUE(hal::parse_url("http://h:70000/", u));
    TEST_ASSERT_EQUAL_UINT16(80, u.port);
    TEST_ASSERT_TRUE(hal::parse_url("https://h:abc", u));
    TEST_ASSERT_EQUAL_UINT16(443, u.port);
}

static void test_parse_url_empty_host_fails(void)
{
    hal::UrlParts u;
    TEST_ASSERT_FALSE(hal::parse_url("", u));
    TEST_ASSERT_FALSE(hal::parse_url("http://", u));
    TEST_ASSERT_FALSE(hal::parse_url("http://:8080/x", u));
    TEST_ASSERT_FALSE(hal::parse_url("http:///path", u));
}

/* ---- utf8_byte_pos ---- */

static void test_utf8_byte_pos_ascii(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, hal::utf8_byte_pos("hello", 0));
    TEST_ASSERT_EQUAL_UINT32(3, hal::utf8_byte_pos("hello", 3));
}

static void test_utf8_byte_pos_multibyte(void)
{
    // "a" (1) + "é" (2) + "€" (3) + U+1F600 (4) + "b"
    const char *s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80" "b";
    TEST_ASSERT_EQUAL_UINT32(1, hal::utf8_byte_pos(s, 1));
    TEST_ASSERT_EQUAL_UINT32(3, hal::utf8_byte_pos(s, 2));
    TEST_ASSERT_EQUAL_UINT32(6, hal::utf8_byte_pos(s, 3));
    TEST_ASSERT_EQUAL_UINT32(10, hal::utf8_byte_pos(s, 4));
}

static void test_utf8_byte_pos_clamps_at_end(void)
{
    TEST_ASSERT_EQUAL_UINT32(3, hal::utf8_byte_pos("abc", 99));
}

static void test_utf8_byte_pos_stray_continuation_is_one_char(void)
{
    TEST_ASSERT_EQUAL_UINT32(2, hal::utf8_byte_pos("\x80" "a", 2));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_json_escape_empty);
    RUN_TEST(test_json_escape_plain_passthrough);
    RUN_TEST(test_json_escape_quote_and_backslash);
    RUN_TEST(test_json_escape_named_control_chars);
    RUN_TEST(test_json_escape_other_control_char_as_u_escape);
    RUN_TEST(test_json_escape_utf8_bytes_pass_through);

    RUN_TEST(test_url_encode_empty);
    RUN_TEST(test_url_encode_unreserved_passthrough);
    RUN_TEST(test_url_encode_space_and_reserved);
    RUN_TEST(test_url_encode_utf8_bytes_percent_encoded);
    RUN_TEST(test_url_encode_hex_digits_are_uppercase);

    RUN_TEST(test_trim_empty_and_all_whitespace);
    RUN_TEST(test_trim_both_ends);
    RUN_TEST(test_trim_no_whitespace_passthrough);

    RUN_TEST(test_parse_url_full);
    RUN_TEST(test_parse_url_scheme_default_ports);
    RUN_TEST(test_parse_url_no_scheme);
    RUN_TEST(test_parse_url_bad_port_keeps_default);
    RUN_TEST(test_parse_url_empty_host_fails);

    RUN_TEST(test_utf8_byte_pos_ascii);
    RUN_TEST(test_utf8_byte_pos_multibyte);
    RUN_TEST(test_utf8_byte_pos_clamps_at_end);
    RUN_TEST(test_utf8_byte_pos_stray_continuation_is_one_char);

    return UNITY_END();
}
