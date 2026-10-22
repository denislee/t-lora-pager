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
#include <vector>

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

/* ------------------------------------------------------ json_array_items */

static void test_array_items_basic(void)
{
    std::vector<std::string> items;
    TEST_ASSERT_TRUE(json_array_items(
        "[1, \"a,b\", {\"x\":[1,2]}, [3,4], true, null]", items));
    TEST_ASSERT_EQUAL(6, (int)items.size());
    TEST_ASSERT_EQUAL_STRING("1", items[0].c_str());
    TEST_ASSERT_EQUAL_STRING("\"a,b\"", items[1].c_str());
    TEST_ASSERT_EQUAL_STRING("{\"x\":[1,2]}", items[2].c_str());
    TEST_ASSERT_EQUAL_STRING("[3,4]", items[3].c_str());
    TEST_ASSERT_EQUAL_STRING("true", items[4].c_str());
    TEST_ASSERT_EQUAL_STRING("null", items[5].c_str());
}

static void test_array_items_escapes(void)
{
    std::vector<std::string> items;
    TEST_ASSERT_TRUE(json_array_items("[\"q\\\"\", \"\\\\\"]", items));
    TEST_ASSERT_EQUAL(2, (int)items.size());
    TEST_ASSERT_EQUAL_STRING("\"q\\\"\"", items[0].c_str());
    TEST_ASSERT_EQUAL_STRING("\"\\\\\"", items[1].c_str());
}

static void test_array_items_empty(void)
{
    std::vector<std::string> items;
    TEST_ASSERT_TRUE(json_array_items("[]", items));
    TEST_ASSERT_EQUAL(0, (int)items.size());

    TEST_ASSERT_TRUE(json_array_items("  [ ]  ", items));
    TEST_ASSERT_EQUAL(0, (int)items.size());
}

static void test_array_items_rejects(void)
{
    static const char *const bad[] = {
        "{}",
        "[1,]",
        "[1",
        "1",
        "[1] x",
        "",
        "[1],\"a\":[2]",
    };
    for (const char *src : bad) {
        std::vector<std::string> items;
        items.push_back("stale");
        TEST_ASSERT_FALSE(json_array_items(src, items));
        TEST_ASSERT_EQUAL(0, (int)items.size());
    }
}

/* ------------------------------------------------------ json_array_spans */

static void test_array_spans_basic(void)
{
    /* [ 1 , "a\"b" ,{"k":[2,3]}, null ] with surrounding whitespace. */
    const std::string t = "  [ 1 , \"a\\\"b\" ,{\"k\":[2,3]}, null ]  ";
    std::vector<JsonSpan> spans;
    TEST_ASSERT_TRUE(json_array_spans(t.data(), t.size(), spans));
    TEST_ASSERT_EQUAL(4, (int)spans.size());
    TEST_ASSERT_EQUAL_STRING("1", t.substr(spans[0].off, spans[0].len).c_str());
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\"", t.substr(spans[1].off, spans[1].len).c_str());
    TEST_ASSERT_EQUAL_STRING("{\"k\":[2,3]}", t.substr(spans[2].off, spans[2].len).c_str());
    TEST_ASSERT_EQUAL_STRING("null", t.substr(spans[3].off, spans[3].len).c_str());
}

static void test_array_spans_matches_items(void)
{
    static const char *const cases[] = {
        "[]",
        " [ ] ",
        "[1,2,3]",
        "[\"x\",{\"a\":1},[1,[2]],true,false,null,-1.5e3]",
        "[ \"q\\\"\" , \"\\\\\" ]",
    };
    for (const char *src : cases) {
        const std::string text(src);
        std::vector<std::string> items;
        std::vector<JsonSpan> spans;
        TEST_ASSERT_TRUE(json_array_items(text, items));
        TEST_ASSERT_TRUE(json_array_spans(text.data(), text.size(), spans));
        TEST_ASSERT_EQUAL((int)items.size(), (int)spans.size());
        for (size_t k = 0; k < items.size(); ++k) {
            TEST_ASSERT_EQUAL_STRING(items[k].c_str(),
                                     text.substr(spans[k].off, spans[k].len).c_str());
        }
    }
}

static void test_array_spans_rejects(void)
{
    static const char *const bad[] = {
        "[1,]", "[1] x", "{}", "", "[", "[1 2]", "[1]]", "]",
    };
    for (const char *src : bad) {
        std::vector<JsonSpan> spans;
        spans.push_back(JsonSpan{1, 1}); /* stale entry must be dropped */
        TEST_ASSERT_FALSE(json_array_spans(src, std::strlen(src), spans));
        TEST_ASSERT_EQUAL(0, (int)spans.size());

        std::vector<std::string> items;
        TEST_ASSERT_FALSE(json_array_items(src, items));
    }
}

static void test_array_spans_depth(void)
{
    for (int k = 1; k <= 20; ++k) {
        std::string text(static_cast<size_t>(k), '[');
        text.append(static_cast<size_t>(k), ']');

        std::vector<JsonSpan> spans;
        std::vector<std::string> items;
        const bool spans_ok = json_array_spans(text.data(), text.size(), spans);
        const bool items_ok = json_array_items(text, items);
        TEST_ASSERT_TRUE(items_ok == spans_ok);
        if (k == 15) {
            TEST_ASSERT_TRUE(spans_ok); /* innermost array at depth 16 */
        }
        if (k == 16) {
            TEST_ASSERT_FALSE(spans_ok); /* depth 17 */
        }
    }
}

/* --------------------------------------------------------- parse_view */

static void test_parse_view_borrows(void)
{
    char buf[] = "{\"a\":\"x\",\"n\":5,\"o\":{\"t\":\"p\",\"w\":3}}";
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse_view(buf, std::strlen(buf)));

    std::string s;
    TEST_ASSERT_TRUE(o.get_string("a", s));
    TEST_ASSERT_EQUAL_STRING("x", s.c_str());

    int64_t n = 0;
    TEST_ASSERT_TRUE(o.get_int("n", n));
    TEST_ASSERT_EQUAL_INT64(5, n);

    JsonObject c;
    TEST_ASSERT_TRUE(o.get_object_view("o", c));
    TEST_ASSERT_TRUE(c.get_string("t", s));
    TEST_ASSERT_EQUAL_STRING("p", s.c_str());
    int64_t w = 0;
    TEST_ASSERT_TRUE(c.get_int("w", w));
    TEST_ASSERT_EQUAL_INT64(3, w);

    TEST_ASSERT_TRUE(o.text() == std::string(buf));

    /* The value bytes are borrowed: an in-place edit is visible through it. */
    char *px = std::strchr(buf + 5, 'x');
    TEST_ASSERT_NOT_NULL(px);
    *px = 'y';
    TEST_ASSERT_TRUE(o.get_string("a", s));
    TEST_ASSERT_EQUAL_STRING("y", s.c_str());

    /* A failed re-parse leaves the previous state untouched. */
    TEST_ASSERT_FALSE(o.parse_view("{\"a\":}", 6));
    TEST_ASSERT_TRUE(o.has("a"));
    TEST_ASSERT_TRUE(o.get_string("a", s));
    TEST_ASSERT_EQUAL_STRING("y", s.c_str());
}

static void test_parse_view_then_parse_owns(void)
{
    char buf[] = "{\"a\":\"x\"}";
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse_view(buf, std::strlen(buf)));
    TEST_ASSERT_TRUE(o.parse(std::string("{\"a\":\"z\"}")));

    std::memset(buf, '?', std::strlen(buf));

    std::string s;
    TEST_ASSERT_TRUE(o.get_string("a", s));
    TEST_ASSERT_EQUAL_STRING("z", s.c_str());

    JsonObject owned;
    TEST_ASSERT_TRUE(owned.parse(std::string("{\"o\":{\"t\":1}}")));
    JsonObject child;
    TEST_ASSERT_TRUE(owned.get_object_view("o", child));
    int64_t t = 0;
    TEST_ASSERT_TRUE(child.get_int("t", t));
    TEST_ASSERT_EQUAL_INT64(1, t);
    TEST_ASSERT_FALSE(owned.get_object_view("missing", child));
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
    RUN_TEST(test_array_items_basic);
    RUN_TEST(test_array_items_escapes);
    RUN_TEST(test_array_items_empty);
    RUN_TEST(test_array_items_rejects);
    RUN_TEST(test_array_spans_basic);
    RUN_TEST(test_array_spans_matches_items);
    RUN_TEST(test_array_spans_rejects);
    RUN_TEST(test_array_spans_depth);
    RUN_TEST(test_parse_view_borrows);
    RUN_TEST(test_parse_view_then_parse_owns);
    return UNITY_END();
}
