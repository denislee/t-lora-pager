/**
 * @file      test_main.cpp
 * @brief     Unity tests for apps/tg_favs.h — the pure codec of the Telegram
 *            favourites list: the `<id>\x1F<title>\x1E` titles, the stored order
 *            and the dropped non-chats.
 *
 * The encode/decode pair is pure (no NVS), so the whole format is covered here.
 */
#include <unity.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "apps/tg_favs.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- tg_favs_decode ---- */

static void test_decode_empty(void) {
    const apps::TgFavs favs = apps::tg_favs_decode("", "");
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(favs.size()));
}

static void test_decode_keeps_stored_order(void) {
    const apps::TgFavs favs = apps::tg_favs_decode("5,3,1", "");
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(favs.size()));
    TEST_ASSERT_TRUE(favs[0].first == 5);
    TEST_ASSERT_TRUE(favs[1].first == 3);
    TEST_ASSERT_TRUE(favs[2].first == 1);
    TEST_ASSERT_EQUAL_STRING("", favs[0].second.c_str());
    TEST_ASSERT_EQUAL_STRING("", favs[1].second.c_str());
    TEST_ASSERT_EQUAL_STRING("", favs[2].second.c_str());
}

static void test_decode_attaches_titles(void) {
    const apps::TgFavs favs = apps::tg_favs_decode(
        "5,-100123,3", "5\x1F" "Five\x1E" "-100123\x1F" "Group\x1E");
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(favs.size()));
    TEST_ASSERT_TRUE(favs[0].first == 5);
    TEST_ASSERT_EQUAL_STRING("Five", favs[0].second.c_str());
    TEST_ASSERT_TRUE(favs[1].first == -100123);
    TEST_ASSERT_EQUAL_STRING("Group", favs[1].second.c_str());
    TEST_ASSERT_TRUE(favs[2].first == 3);
    TEST_ASSERT_EQUAL_STRING("", favs[2].second.c_str());
}

static void test_decode_drops_non_chats(void) {
    const apps::TgFavs favs = apps::tg_favs_decode("0,abc,7,,8", "");
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(favs.size()));
    TEST_ASSERT_TRUE(favs[0].first == 7);
    TEST_ASSERT_TRUE(favs[1].first == 8);
}

static void test_decode_ignores_foreign_and_malformed_titles(void) {
    const apps::TgFavs favs = apps::tg_favs_decode(
        "7", "9\x1F" "nope\x1E" "garbage\x1E");
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(favs.size()));
    TEST_ASSERT_TRUE(favs[0].first == 7);
    TEST_ASSERT_EQUAL_STRING("", favs[0].second.c_str());
}

static void test_decode_tolerates_unterminated_record(void) {
    const apps::TgFavs favs = apps::tg_favs_decode("7", "7\x1F" "x");
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(favs.size()));
    TEST_ASSERT_TRUE(favs[0].first == 7);
    TEST_ASSERT_EQUAL_STRING("x", favs[0].second.c_str());
}

/* ---- tg_favs_encode ---- */

static void test_encode_keeps_given_order(void) {
    apps::TgFavs favs;
    favs.emplace_back(5, "Five");
    favs.emplace_back(-100123, "Group");
    favs.emplace_back(3, "");

    std::string ids;
    std::string titles;
    apps::tg_favs_encode(favs, ids, titles);

    TEST_ASSERT_EQUAL_STRING("5,-100123,3", ids.c_str());
    TEST_ASSERT_EQUAL_STRING("5\x1F" "Five\x1E" "-100123\x1F" "Group\x1E",
                             titles.c_str());
}

static void test_encode_replaces_previous_output(void) {
    std::string ids = "stale";
    std::string titles = "stale";
    apps::tg_favs_encode(apps::TgFavs(), ids, titles);

    TEST_ASSERT_EQUAL_STRING("", ids.c_str());
    TEST_ASSERT_EQUAL_STRING("", titles.c_str());
}

/* ---- round trip ---- */

static void test_round_trip(void) {
    apps::TgFavs favs;
    favs.emplace_back(9, "Nine");
    favs.emplace_back(2, "Two");
    favs.emplace_back(40, "");

    std::string ids;
    std::string titles;
    apps::tg_favs_encode(favs, ids, titles);
    const apps::TgFavs again = apps::tg_favs_decode(ids, titles);

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(favs.size()),
                             static_cast<uint32_t>(again.size()));
    for (size_t i = 0; i < favs.size(); ++i) {
        TEST_ASSERT_TRUE(favs[i].first == again[i].first);
        TEST_ASSERT_EQUAL_STRING(favs[i].second.c_str(), again[i].second.c_str());
    }
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_decode_empty);
    RUN_TEST(test_decode_keeps_stored_order);
    RUN_TEST(test_decode_attaches_titles);
    RUN_TEST(test_decode_drops_non_chats);
    RUN_TEST(test_decode_ignores_foreign_and_malformed_titles);
    RUN_TEST(test_decode_tolerates_unterminated_record);
    RUN_TEST(test_encode_keeps_given_order);
    RUN_TEST(test_encode_replaces_previous_output);
    RUN_TEST(test_round_trip);

    return UNITY_END();
}
