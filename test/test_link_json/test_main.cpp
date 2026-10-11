/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/link/link_json.{h,cpp}.
 *
 * Exercises the compact JSON writer (separator/escaping rules) and the
 * RFC 8259 object validator/reader (member indexing, escape decoding and the
 * rejection of malformed input) on the host, where cJSON is not available.
 */
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "hal/link/link_json.h"

using namespace pagerlink;

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------ writer */

static void test_writer_nested_object_array(void)
{
    JsonWriter w;
    w.begin_object()
        .key("t").int_val(42)
        .key("a").begin_array().int_val(1).int_val(2).end_array()
        .end_object();
    TEST_ASSERT_EQUAL_STRING("{\"t\":42,\"a\":[1,2]}", w.str().c_str());
    TEST_ASSERT_FALSE(w.empty());
}

static void test_writer_string_escapes(void)
{
    JsonWriter w;
    /* a " b \ c LF 0x01 */
    const char *in = "a\"b\\c\n\x01";
    w.str_val(in);
    /* -> "a\"b\\c\n\u0001" */
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\n\\u0001\"", w.str().c_str());
}

static void test_writer_null_for_nullptr(void)
{
    JsonWriter w;
    w.str_val(nullptr);
    TEST_ASSERT_EQUAL_STRING("null", w.str().c_str());
}

static void test_writer_integers(void)
{
    JsonWriter w;
    w.int_val(-5);
    TEST_ASSERT_EQUAL_STRING("-5", w.str().c_str());

    w.clear();
    w.int_val(INT64_MIN);
    TEST_ASSERT_EQUAL_STRING("-9223372036854775808", w.str().c_str());
}

/* ---------------------------------------------------------------- parse/get */

static void test_parse_and_get_members(void)
{
    const char *src =
        "{\"s\":\"hi\",\"i\":-7,\"d\":3.5,\"b\":true,\"n\":null,"
        "\"o\":{\"x\":1},\"a\":[1,2,3]}";
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse(src));
    TEST_ASSERT_EQUAL_INT(7, (int)o.size());
    TEST_ASSERT_TRUE(o.has("s"));
    TEST_ASSERT_TRUE(o.has("a"));

    std::string s;
    TEST_ASSERT_TRUE(o.get_string("s", s));
    TEST_ASSERT_EQUAL_STRING("hi", s.c_str());

    int64_t i = 0;
    TEST_ASSERT_TRUE(o.get_int("i", i));
    TEST_ASSERT_EQUAL_INT64(-7, i);

    double d = 0.0;
    TEST_ASSERT_TRUE(o.get_double("d", d));
    TEST_ASSERT_TRUE(std::fabs(d - 3.5) < 1e-9);

    bool b = false;
    TEST_ASSERT_TRUE(o.get_bool("b", b));
    TEST_ASSERT_TRUE(b);
    TEST_ASSERT_TRUE(o.is_null("n"));

    JsonObject nested;
    TEST_ASSERT_TRUE(o.get_object("o", nested));
    int64_t x = 0;
    TEST_ASSERT_TRUE(nested.get_int("x", x));
    TEST_ASSERT_EQUAL_INT64(1, x);

    std::string raw;
    TEST_ASSERT_TRUE(o.get_raw("a", raw));
    TEST_ASSERT_EQUAL_STRING("[1,2,3]", raw.c_str());
}

static void test_get_string_unicode(void)
{
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse("{\"e\":\"\\u00e9\"}"));
    std::string e;
    TEST_ASSERT_TRUE(o.get_string("e", e));
    TEST_ASSERT_EQUAL_STRING("\xc3\xa9", e.c_str()); /* é */

    JsonObject p;
    TEST_ASSERT_TRUE(p.parse("{\"f\":\"\\ud83d\\ude00\"}"));
    std::string f;
    TEST_ASSERT_TRUE(p.get_string("f", f));
    TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x98\x80", f.c_str()); /* 😀 */
}

static void test_get_int_rejects_non_integers(void)
{
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse("{\"a\":1.5,\"b\":1e3,\"c\":\"5\"}"));
    int64_t v = 0;
    TEST_ASSERT_FALSE(o.get_int("a", v));
    TEST_ASSERT_FALSE(o.get_int("b", v));
    TEST_ASSERT_FALSE(o.get_int("c", v));
}

static void test_missing_keys(void)
{
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse("{\"a\":1}"));
    std::string s;
    int64_t i = 0;
    double d = 0.0;
    bool b = false;
    JsonObject n;
    TEST_ASSERT_FALSE(o.has("zzz"));
    TEST_ASSERT_FALSE(o.get_raw("zzz", s));
    TEST_ASSERT_FALSE(o.get_string("zzz", s));
    TEST_ASSERT_FALSE(o.get_int("zzz", i));
    TEST_ASSERT_FALSE(o.get_double("zzz", d));
    TEST_ASSERT_FALSE(o.get_bool("zzz", b));
    TEST_ASSERT_FALSE(o.is_null("zzz"));
    TEST_ASSERT_FALSE(o.get_object("zzz", n));
}

static void test_parse_rejects(void)
{
    static const char *const bad[] = {
        "",
        "[]",
        "{",
        "{\"a\":1,}",
        "{\"a\":01}",
        "{\"a\":1} x",
        "{\"a\":\"\x01\"}",
    };
    for (const char *src : bad) {
        JsonObject o;
        TEST_ASSERT_FALSE(o.parse(src));
    }
}

static void test_lone_surrogate_parses_but_does_not_decode(void)
{
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse("{\"a\":\"\\ud800\"}"));
    std::string s;
    TEST_ASSERT_FALSE(o.get_string("a", s));
}

static std::string nest_arrays(int k)
{
    std::string s = "{\"a\":";
    s.append(static_cast<size_t>(k), '[');
    s.append(static_cast<size_t>(k), ']');
    s += "}";
    return s;
}

static void test_depth_limit(void)
{
    JsonObject o;
    const std::string ok = nest_arrays(15); /* innermost container at depth 16 */
    TEST_ASSERT_TRUE(o.parse(ok));

    const std::string bad = nest_arrays(16); /* depth 17 */
    TEST_ASSERT_FALSE(o.parse(bad));
}

static void test_whitespace_around_accepted(void)
{
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse("  \n\t {\"a\": 1 , \"b\" : [ 2 , 3 ] } \r\n "));
    int64_t a = 0;
    TEST_ASSERT_TRUE(o.get_int("a", a));
    TEST_ASSERT_EQUAL_INT64(1, a);
}

static void test_duplicate_key_last_wins(void)
{
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse("{\"a\":1,\"a\":2}"));
    int64_t a = 0;
    TEST_ASSERT_TRUE(o.get_int("a", a));
    TEST_ASSERT_EQUAL_INT64(2, a);
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_writer_nested_object_array);
    RUN_TEST(test_writer_string_escapes);
    RUN_TEST(test_writer_null_for_nullptr);
    RUN_TEST(test_writer_integers);
    RUN_TEST(test_parse_and_get_members);
    RUN_TEST(test_get_string_unicode);
    RUN_TEST(test_get_int_rejects_non_integers);
    RUN_TEST(test_missing_keys);
    RUN_TEST(test_parse_rejects);
    RUN_TEST(test_lone_surrogate_parses_but_does_not_decode);
    RUN_TEST(test_depth_limit);
    RUN_TEST(test_whitespace_around_accepted);
    RUN_TEST(test_duplicate_key_last_wins);
    return UNITY_END();
}
