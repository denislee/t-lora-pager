/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/companion_msg.{h,cpp} (PROTOCOL.md §6.5).
 *
 * Every case of protocol/vectors/messaging.json runs through the pager's
 * parser: the same verdict (and, for a valid one, the same normalized fields)
 * as the Python oracle. Then the inbox ring: dedupe on id, the cap, the exact
 * /inbox.jsonl text, a round trip, and recovery from a torn last line. Then
 * the fake-sleep delivery policy and the UTF-8 cut used by clip.push.
 */
#include <unity.h>

#include <string>
#include <vector>

#include "hal/companion_msg.h"
#include "hal/link/link_json.h"
#include "messaging_vectors_gen.h" /* kMessagingVectorsJson: protocol/vectors/messaging.json */

using namespace hal::msg;
using pagerlink::JsonObject;

void setUp(void) {}
void tearDown(void) {}

static JsonObject root()
{
    JsonObject r;
    TEST_ASSERT_TRUE(r.parse(kMessagingVectorsJson));
    return r;
}

static std::vector<std::string> array_of(const JsonObject &o, const char *key)
{
    std::string raw;
    std::vector<std::string> items;
    TEST_ASSERT_TRUE(o.get_raw(key, raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, items));
    return items;
}

static Prio prio_of(const std::string &s)
{
    return s == "low" ? Prio::Low : s == "high" ? Prio::High : Prio::Normal;
}

static void test_vector_cases(void)
{
    const JsonObject r = root();
    const std::vector<std::string> cases = array_of(r, "cases");
    TEST_ASSERT_TRUE(cases.size() > 20);
    for (const std::string &elem : cases) {
        JsonObject cs;
        TEST_ASSERT_TRUE(cs.parse(elem));
        std::string name, method, params_json;
        TEST_ASSERT_TRUE(cs.get_string("name", name));
        TEST_ASSERT_TRUE(cs.get_string("method", method));
        TEST_ASSERT_TRUE(cs.get_string("params_json", params_json));
        JsonObject params;
        TEST_ASSERT_TRUE_MESSAGE(params.parse(params_json), name.c_str());

        std::string why;
        bool ok = false;
        JsonObject want_ok, want_err;
        const bool expect_ok = cs.get_object("ok", want_ok);
        if (!expect_ok) TEST_ASSERT_TRUE_MESSAGE(cs.get_object("error", want_err), name.c_str());

        if (method == "notify.post") {
            NotifyPost np;
            ok = parse_notify_post(params, np, why);
            if (ok && expect_ok) {
                std::string s;
                int64_t ts = 0;
                TEST_ASSERT_TRUE(want_ok.get_string("title", s));
                TEST_ASSERT_EQUAL_STRING_MESSAGE(s.c_str(), np.title.c_str(), name.c_str());
                TEST_ASSERT_TRUE(want_ok.get_string("body", s));
                TEST_ASSERT_EQUAL_STRING_MESSAGE(s.c_str(), np.body.c_str(), name.c_str());
                TEST_ASSERT_TRUE(want_ok.get_string("id", s));
                TEST_ASSERT_EQUAL_STRING_MESSAGE(s.c_str(), np.id.c_str(), name.c_str());
                TEST_ASSERT_TRUE(want_ok.get_string("src", s));
                TEST_ASSERT_EQUAL_STRING_MESSAGE(s.c_str(), np.src.c_str(), name.c_str());
                TEST_ASSERT_TRUE(want_ok.get_int("ts", ts));
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(ts), np.ts, name.c_str());
                TEST_ASSERT_TRUE(want_ok.get_string("prio", s));
                TEST_ASSERT_EQUAL_STRING_MESSAGE(s.c_str(), prio_name(np.prio), name.c_str());
            }
        } else if (method == "clip.push") {
            std::string text;
            ok = parse_clip_push(params, text, why);
            if (ok && expect_ok) {
                std::string s;
                TEST_ASSERT_TRUE(want_ok.get_string("text", s));
                TEST_ASSERT_TRUE_MESSAGE(s == text, name.c_str());
            }
        } else if (method == "text.send") {
            std::string text;
            Target t = Target::Kb;
            ok = parse_text_send(params, text, t, why);
            if (ok && expect_ok) {
                std::string s;
                TEST_ASSERT_TRUE(want_ok.get_string("text", s));
                TEST_ASSERT_TRUE_MESSAGE(s == text, name.c_str());
                TEST_ASSERT_TRUE(want_ok.get_string("target", s));
                TEST_ASSERT_EQUAL_STRING_MESSAGE(s.c_str(), t == Target::Editor ? "editor" : "kb",
                                                 name.c_str());
            }
        } else {
            TEST_FAIL_MESSAGE(method.c_str());
        }

        TEST_ASSERT_EQUAL_MESSAGE(expect_ok, ok, name.c_str());
        if (!expect_ok) {
            std::string msg;
            TEST_ASSERT_TRUE(want_err.get_string("msg", msg));
            TEST_ASSERT_EQUAL_STRING_MESSAGE(msg.c_str(), why.c_str(), name.c_str());
        }
    }
}

static InboxEntry entry_of(const std::string &elem)
{
    JsonObject o;
    TEST_ASSERT_TRUE(o.parse(elem));
    InboxEntry e;
    o.get_string("id", e.id);
    o.get_string("kind", e.kind);
    o.get_string("title", e.title);
    o.get_string("body", e.body);
    o.get_string("src", e.src);
    int64_t v = 0;
    if (o.get_int("ts", v)) e.ts = static_cast<uint32_t>(v);
    if (o.get_int("rx", v)) e.rx = static_cast<uint32_t>(v);
    std::string p;
    if (o.get_string("prio", p)) e.prio = prio_of(p);
    o.get_bool("read", e.read);
    return e;
}

static void test_inbox_ring(void)
{
    const JsonObject r = root();
    JsonObject inbox;
    TEST_ASSERT_TRUE(r.get_object("inbox", inbox));
    int64_t cap = 0;
    TEST_ASSERT_TRUE(inbox.get_int("cap", cap));
    const std::vector<std::string> adds = array_of(inbox, "adds");
    const std::vector<std::string> added = array_of(inbox, "added");
    TEST_ASSERT_EQUAL(adds.size(), added.size());

    std::vector<InboxEntry> ring;
    for (size_t i = 0; i < adds.size(); i++) {
        const bool got = inbox_add(ring, entry_of(adds[i]), static_cast<size_t>(cap));
        TEST_ASSERT_EQUAL_MESSAGE(added[i] == "true", got, adds[i].c_str());
    }

    std::string want;
    TEST_ASSERT_TRUE(inbox.get_string("jsonl", want));
    const std::string text = inbox_encode(ring);
    TEST_ASSERT_EQUAL_STRING(want.c_str(), text.c_str());

    int64_t unread = 0;
    TEST_ASSERT_TRUE(inbox.get_int("unread", unread));
    TEST_ASSERT_EQUAL(static_cast<size_t>(unread), inbox_unread(ring));

    // Round trip.
    std::vector<InboxEntry> back;
    inbox_decode(text, back);
    TEST_ASSERT_EQUAL_STRING(text.c_str(), inbox_encode(back).c_str());

    // A torn write and an entry without a title are skipped.
    std::string damaged;
    TEST_ASSERT_TRUE(inbox.get_string("damaged_jsonl", damaged));
    inbox_decode(damaged, back);
    const std::vector<std::string> titles = array_of(inbox, "damaged_titles");
    TEST_ASSERT_EQUAL(titles.size(), back.size());
    for (size_t i = 0; i < titles.size(); i++) {
        TEST_ASSERT_EQUAL_STRING(("\"" + back[i].title + "\"").c_str(), titles[i].c_str());
    }

    // decode keeps only the newest `cap`.
    inbox_decode(text, back, 1);
    TEST_ASSERT_EQUAL(1, back.size());
    TEST_ASSERT_EQUAL_STRING("four", back[0].title.c_str());
}

static void test_delivery_policy(void)
{
    const JsonObject r = root();
    for (const std::string &elem : array_of(r, "delivery")) {
        JsonObject d;
        TEST_ASSERT_TRUE(d.parse(elem));
        std::string p;
        bool fs = false, cw = false, banner = false, haptic = false, wake = false;
        TEST_ASSERT_TRUE(d.get_string("prio", p));
        TEST_ASSERT_TRUE(d.get_bool("fake_sleep", fs));
        TEST_ASSERT_TRUE(d.get_bool("can_wake", cw));
        TEST_ASSERT_TRUE(d.get_bool("banner", banner));
        TEST_ASSERT_TRUE(d.get_bool("haptic", haptic));
        TEST_ASSERT_TRUE(d.get_bool("wake", wake));
        const Delivery got = delivery_for(prio_of(p), fs, cw);
        TEST_ASSERT_EQUAL_MESSAGE(banner, got.banner, elem.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(haptic, got.haptic, elem.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(wake, got.wake, elem.c_str());
    }
}

static void test_utf8_prefix(void)
{
    const std::string s = "ab\xC3\xA9" "cd";  // a b é c d
    TEST_ASSERT_EQUAL(6, utf8_prefix_len(s, 10));
    TEST_ASSERT_EQUAL(6, utf8_prefix_len(s, 6));
    TEST_ASSERT_EQUAL(4, utf8_prefix_len(s, 4));
    TEST_ASSERT_EQUAL(2, utf8_prefix_len(s, 3));  // never splits é
    TEST_ASSERT_EQUAL(0, utf8_prefix_len(s, 0));
    const std::string emoji = "\xF0\x9F\x98\x80";
    TEST_ASSERT_EQUAL(0, utf8_prefix_len(emoji, 3));
}

static void test_invalid_utf8_refused(void)
{
    // JSON text can carry raw bytes that are not UTF-8; the rules refuse them.
    JsonObject p;
    const std::string json = std::string("{\"title\":\"a\xFF\"}");
    TEST_ASSERT_TRUE(p.parse(json));
    NotifyPost np;
    std::string why;
    TEST_ASSERT_FALSE(parse_notify_post(p, np, why));
    TEST_ASSERT_EQUAL_STRING("title: not valid UTF-8", why.c_str());
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_vector_cases);
    RUN_TEST(test_inbox_ring);
    RUN_TEST(test_delivery_policy);
    RUN_TEST(test_utf8_prefix);
    RUN_TEST(test_invalid_utf8_refused);
    return UNITY_END();
}
