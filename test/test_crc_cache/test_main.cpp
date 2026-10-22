/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/link/crc_cache.{h,cpp}.
 *
 * Covers the pure LRU that caches whole-file CRC-32s for the PagerLink file
 * service: a lookup hits only when every field of the key matches (volume,
 * path, size, mtime and filesystem generation), a hit becomes the most recent
 * entry, storing replaces the entry for the same (vol, path) and evicts the
 * least recent past capacity, and a zero-capacity cache stores nothing.
 */
#include <unity.h>

#include <cstdint>
#include <string>

#include "hal/link/crc_cache.h"

using namespace pagerlink;

void setUp(void) {}
void tearDown(void) {}

/* --- helpers ------------------------------------------------------------- */

static CrcKey make_key(uint8_t vol, const char *path, uint32_t size, uint32_t mtime,
                       uint32_t gen) {
    return CrcKey{vol, std::string(path), size, mtime, gen};
}

/* --- tests --------------------------------------------------------------- */

static void test_miss_on_empty(void) {
    CrcCache cache(4);
    uint32_t crc = 0xFFFFFFFFu;

    TEST_ASSERT_FALSE(cache.lookup(make_key(0, "/notes/a.txt", 10, 100, 5), crc));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, crc);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(cache.size()));
}

static void test_hit_exact_key(void) {
    CrcCache cache(4);
    const CrcKey k = make_key(0, "/notes/a.txt", 10, 100, 5);
    cache.store(k, 0xDEADBEEFu);

    uint32_t crc = 0;
    TEST_ASSERT_TRUE(cache.lookup(k, crc));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, crc);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(cache.size()));
}

static void test_generation_invalidates(void) {
    CrcCache cache(4);
    cache.store(make_key(0, "/notes/a.txt", 10, 100, 5), 0xDEADBEEFu);

    uint32_t crc = 0;
    TEST_ASSERT_FALSE(cache.lookup(make_key(0, "/notes/a.txt", 10, 100, 6), crc));
}

static void test_size_invalidates(void) {
    CrcCache cache(4);
    cache.store(make_key(0, "/notes/a.txt", 10, 100, 5), 0xDEADBEEFu);

    uint32_t crc = 0;
    TEST_ASSERT_FALSE(cache.lookup(make_key(0, "/notes/a.txt", 11, 100, 5), crc));
}

static void test_mtime_invalidates(void) {
    CrcCache cache(4);
    cache.store(make_key(0, "/notes/a.txt", 10, 100, 5), 0xDEADBEEFu);

    uint32_t crc = 0;
    TEST_ASSERT_FALSE(cache.lookup(make_key(0, "/notes/a.txt", 10, 101, 5), crc));
}

static void test_vol_and_path_distinct(void) {
    CrcCache cache(4);
    const CrcKey k = make_key(0, "/notes/a.txt", 10, 100, 5);
    const uint32_t crc_value = 0xDEADBEEFu;
    cache.store(k, crc_value);

    uint32_t crc = 0;
    TEST_ASSERT_FALSE(cache.lookup(make_key(1, "/notes/a.txt", 10, 100, 5), crc));
    TEST_ASSERT_FALSE(cache.lookup(make_key(0, "/notes/b.txt", 10, 100, 5), crc));

    /* The original entry is still a hit. */
    TEST_ASSERT_TRUE(cache.lookup(k, crc));
    TEST_ASSERT_EQUAL_UINT32(crc_value, crc);
}

static void test_store_replaces_same_path(void) {
    CrcCache cache(4);
    cache.store(make_key(0, "/notes/a.txt", 10, 100, 5), 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(cache.size()));

    cache.store(make_key(0, "/notes/a.txt", 10, 100, 6), 2u);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(cache.size()));

    uint32_t crc = 0;
    TEST_ASSERT_TRUE(cache.lookup(make_key(0, "/notes/a.txt", 10, 100, 6), crc));
    TEST_ASSERT_EQUAL_UINT32(2u, crc);
    TEST_ASSERT_FALSE(cache.lookup(make_key(0, "/notes/a.txt", 10, 100, 5), crc));
}

static void test_lru_eviction(void) {
    CrcCache cache(2);
    const CrcKey a = make_key(0, "/notes/a.txt", 10, 100, 5);
    const CrcKey b = make_key(0, "/notes/b.txt", 20, 200, 5);
    const CrcKey c = make_key(0, "/notes/c.txt", 30, 300, 5);
    cache.store(a, 0xAAAA0001u);
    cache.store(b, 0xBBBB0002u);

    /* Look A up so B becomes the least recent entry. */
    uint32_t crc = 0;
    TEST_ASSERT_TRUE(cache.lookup(a, crc));
    TEST_ASSERT_EQUAL_UINT32(0xAAAA0001u, crc);

    cache.store(c, 0xCCCC0003u);
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(cache.size()));

    TEST_ASSERT_FALSE(cache.lookup(b, crc));
    TEST_ASSERT_TRUE(cache.lookup(a, crc));
    TEST_ASSERT_EQUAL_UINT32(0xAAAA0001u, crc);
    TEST_ASSERT_TRUE(cache.lookup(c, crc));
    TEST_ASSERT_EQUAL_UINT32(0xCCCC0003u, crc);
}

static void test_evict_and_clear(void) {
    CrcCache cache(4);
    const CrcKey a = make_key(0, "/notes/a.txt", 10, 100, 5);
    const CrcKey b = make_key(0, "/notes/b.txt", 20, 200, 5);
    cache.store(a, 1u);
    cache.store(b, 2u);

    cache.evict(0, "/notes/a.txt");
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(cache.size()));

    uint32_t crc = 0;
    TEST_ASSERT_FALSE(cache.lookup(a, crc));
    TEST_ASSERT_TRUE(cache.lookup(b, crc));
    TEST_ASSERT_EQUAL_UINT32(2u, crc);

    cache.clear();
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(cache.size()));
    TEST_ASSERT_FALSE(cache.lookup(b, crc));
}

static void test_zero_capacity(void) {
    CrcCache cache(0);
    const CrcKey k = make_key(0, "/notes/a.txt", 10, 100, 5);
    cache.store(k, 0xDEADBEEFu);

    uint32_t crc = 0;
    TEST_ASSERT_FALSE(cache.lookup(k, crc));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(cache.size()));
}

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_miss_on_empty);
    RUN_TEST(test_hit_exact_key);
    RUN_TEST(test_generation_invalidates);
    RUN_TEST(test_size_invalidates);
    RUN_TEST(test_mtime_invalidates);
    RUN_TEST(test_vol_and_path_distinct);
    RUN_TEST(test_store_replaces_same_path);
    RUN_TEST(test_lru_eviction);
    RUN_TEST(test_evict_and_clear);
    RUN_TEST(test_zero_capacity);
    return UNITY_END();
}
