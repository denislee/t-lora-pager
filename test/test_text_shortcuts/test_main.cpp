/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/text_shortcuts.{h,cpp} — the iOS "." shortcut
 *            the notes editor applies on a second space (word → word. ).
 *
 * The predicate is pure (no Arduino / LVGL): it only looks at the text left of
 * the cursor, so every branch of the word-ending check is covered here.
 */
#include <unity.h>

#include "hal/text_shortcuts.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- double_space_period ---- */

static void test_double_space_after_word(void)
{
    TEST_ASSERT_TRUE(hal::double_space_period("word ", 5));
}

static void test_double_space_after_digit(void)
{
    TEST_ASSERT_TRUE(hal::double_space_period("abc9 ", 5));
}

static void test_double_space_after_paren(void)
{
    TEST_ASSERT_TRUE(hal::double_space_period("end) ", 5));
}

static void test_double_space_after_quote(void)
{
    TEST_ASSERT_TRUE(hal::double_space_period("say\" ", 5));
}

static void test_double_space_after_accented_letter(void)
{
    // "café " — the é tail byte (0xA9) counts as a letter.
    TEST_ASSERT_TRUE(hal::double_space_period("caf\xC3\xA9 ", 6));
}

static void test_double_space_empty_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period("", 0));
}

static void test_double_space_single_space_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period(" ", 1));
}

static void test_double_space_single_letter_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period("a", 1));
}

static void test_double_space_no_trailing_space_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period("word", 4));
}

static void test_double_space_after_period_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period("word. ", 6));
}

static void test_double_space_after_two_spaces_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period("word,  ", 7));
}

static void test_double_space_after_exclamation_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period("word! ", 6));
}

static void test_double_space_after_newline_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period("word\n ", 6));
}

static void test_double_space_null_is_false(void)
{
    TEST_ASSERT_FALSE(hal::double_space_period(nullptr, 0));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_double_space_after_word);
    RUN_TEST(test_double_space_after_digit);
    RUN_TEST(test_double_space_after_paren);
    RUN_TEST(test_double_space_after_quote);
    RUN_TEST(test_double_space_after_accented_letter);
    RUN_TEST(test_double_space_empty_is_false);
    RUN_TEST(test_double_space_single_space_is_false);
    RUN_TEST(test_double_space_single_letter_is_false);
    RUN_TEST(test_double_space_no_trailing_space_is_false);
    RUN_TEST(test_double_space_after_period_is_false);
    RUN_TEST(test_double_space_after_two_spaces_is_false);
    RUN_TEST(test_double_space_after_exclamation_is_false);
    RUN_TEST(test_double_space_after_newline_is_false);
    RUN_TEST(test_double_space_null_is_false);

    return UNITY_END();
}
