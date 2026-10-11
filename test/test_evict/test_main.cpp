/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/evict.h.
 *
 * Pins down the ordering that makes eviction safe: the internal copy is
 * removed only after the SD write reports success. A failed write must leave
 * the original in place — that is the whole point of the safety net.
 */
#include <unity.h>
#include <vector>

#include "hal/evict.h"

void setUp(void) {}
void tearDown(void) {}

static void test_moved_when_copy_succeeds(void)
{
    const std::vector<uint8_t> src = {1, 2, 3};
    std::vector<uint8_t> written;
    int read_calls = 0;
    int write_calls = 0;
    int remove_calls = 0;

    hal::EvictResult r = hal::evict_one(
        [&](std::vector<uint8_t> &out) { read_calls++; out = src; return true; },
        [&](const std::vector<uint8_t> &in) { write_calls++; written = in; return true; },
        [&]() { remove_calls++; });

    TEST_ASSERT_EQUAL((int)hal::EvictResult::Moved, (int)r);
    TEST_ASSERT_EQUAL(1, read_calls);
    TEST_ASSERT_EQUAL(1, write_calls);
    TEST_ASSERT_EQUAL(1, remove_calls);
    TEST_ASSERT_EQUAL(3, (int)written.size());
    TEST_ASSERT_EQUAL_UINT8(1, written[0]);
    TEST_ASSERT_EQUAL_UINT8(2, written[1]);
    TEST_ASSERT_EQUAL_UINT8(3, written[2]);
}

static void test_kept_when_copy_fails(void)
{
    const std::vector<uint8_t> src = {1, 2, 3};
    int write_calls = 0;
    int remove_calls = 0;

    hal::EvictResult r = hal::evict_one(
        [&](std::vector<uint8_t> &out) { out = src; return true; },
        [&](const std::vector<uint8_t> &) { write_calls++; return false; },
        [&]() { remove_calls++; });

    TEST_ASSERT_EQUAL((int)hal::EvictResult::CopyFailed, (int)r);
    TEST_ASSERT_EQUAL(1, write_calls);
    TEST_ASSERT_EQUAL(0, remove_calls); /* the data-safety edge: source kept */
}

static void test_nothing_when_read_fails(void)
{
    int write_calls = 0;
    int remove_calls = 0;

    hal::EvictResult r = hal::evict_one(
        [&](std::vector<uint8_t> &) { return false; },
        [&](const std::vector<uint8_t> &) { write_calls++; return true; },
        [&]() { remove_calls++; });

    TEST_ASSERT_EQUAL((int)hal::EvictResult::ReadFailed, (int)r);
    TEST_ASSERT_EQUAL(0, write_calls);
    TEST_ASSERT_EQUAL(0, remove_calls);
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_moved_when_copy_succeeds);
    RUN_TEST(test_kept_when_copy_fails);
    RUN_TEST(test_nothing_when_read_fails);
    return UNITY_END();
}
