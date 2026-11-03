/**
 * @file      test_main.cpp
 * @brief     Unit tests for apps/kitchen_crank.h.
 *
 * Pins down where the wheel puts the Kitchen's tools: the knife is clamped to
 * its travel, and the peeler runs only forward down a strip's stroke and back
 * up through its return, resting at the bottom and at the top.
 */
#include <unity.h>

#include "apps/kitchen_crank.h"

constexpr float PERIOD = 6.0f;
constexpr float PEEL_END = 0.8f;
constexpr float TRAVEL = 3.0f;

void setUp(void) {}
void tearDown(void) {}

static void test_knife_down_moves_down(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, kitchen_crank::knife_target(0.0f, 1.0f, TRAVEL));
}

static void test_knife_up_moves_up(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, kitchen_crank::knife_target(2.0f, -1.0f, TRAVEL));
}

static void test_knife_stops_at_the_board(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, kitchen_crank::knife_target(2.5f, 1.0f, TRAVEL));
}

static void test_knife_stops_at_the_top(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, kitchen_crank::knife_target(0.5f, -1.0f, TRAVEL));
}

static void test_knife_zero_notches_is_unchanged(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, kitchen_crank::knife_target(1.5f, 0.0f, TRAVEL));
}

static void test_peeler_down_the_stroke(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, kitchen_crank::peeler_target(0.0f, 1.0f, PERIOD, PEEL_END));
}

static void test_peeler_up_the_stroke(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, kitchen_crank::peeler_target(2.0f, -1.0f, PERIOD, PEEL_END));
}

static void test_peeler_stops_at_the_start(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, kitchen_crank::peeler_target(0.0f, -1.0f, PERIOD, PEEL_END));
}

static void test_peeler_stops_at_the_bottom(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.8f, kitchen_crank::peeler_target(4.0f, 3.0f, PERIOD, PEEL_END));
}

static void test_peeler_rests_at_the_bottom(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.8f, kitchen_crank::peeler_target(4.8f, 1.0f, PERIOD, PEEL_END));
}

static void test_peeler_lifts_through_the_return(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.8f, kitchen_crank::peeler_target(4.8f, -1.0f, PERIOD, PEEL_END));
}

static void test_peeler_stops_at_the_top(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.0f, kitchen_crank::peeler_target(5.8f, -1.0f, PERIOD, PEEL_END));
}

static void test_peeler_returns_to_the_bottom(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.8f, kitchen_crank::peeler_target(5.8f, 3.0f, PERIOD, PEEL_END));
}

static void test_peeler_top_is_the_next_strips_start(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.0f, kitchen_crank::peeler_target(6.0f, -1.0f, PERIOD, PEEL_END));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.0f, kitchen_crank::peeler_target(6.0f, 1.0f, PERIOD, PEEL_END));
}

static void test_peeler_zero_notches_is_unchanged(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, kitchen_crank::peeler_target(3.0f, 0.0f, PERIOD, PEEL_END));
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_knife_down_moves_down);
    RUN_TEST(test_knife_up_moves_up);
    RUN_TEST(test_knife_stops_at_the_board);
    RUN_TEST(test_knife_stops_at_the_top);
    RUN_TEST(test_knife_zero_notches_is_unchanged);
    RUN_TEST(test_peeler_down_the_stroke);
    RUN_TEST(test_peeler_up_the_stroke);
    RUN_TEST(test_peeler_stops_at_the_start);
    RUN_TEST(test_peeler_stops_at_the_bottom);
    RUN_TEST(test_peeler_rests_at_the_bottom);
    RUN_TEST(test_peeler_lifts_through_the_return);
    RUN_TEST(test_peeler_stops_at_the_top);
    RUN_TEST(test_peeler_returns_to_the_bottom);
    RUN_TEST(test_peeler_top_is_the_next_strips_start);
    RUN_TEST(test_peeler_zero_notches_is_unchanged);
    return UNITY_END();
}
