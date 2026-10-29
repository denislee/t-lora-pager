/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/ota_state.{h,cpp} (PROTOCOL.md §6.7) and the
 *            streaming SHA-256 the OTA image check uses.
 */
#include <unity.h>

#include <cstring>
#include <string>

#include "hal/ota_state.h"
#include "hal/pbkdf2_sha256.h"

using namespace hal;

void setUp(void) {}
void tearDown(void) {}

static void test_no_pending_does_nothing(void)
{
    OtaBootState s;
    s.boots = 7;  // stale count without pending is cleared
    TEST_ASSERT_EQUAL(static_cast<int>(OtaBootAction::None), static_cast<int>(ota_on_boot(s)));
    TEST_ASSERT_FALSE(s.pending);
    TEST_ASSERT_EQUAL(0, s.boots);
}

static void test_rollback_on_third_unconfirmed_boot(void)
{
    OtaBootState s;
    ota_mark_applied(s);
    TEST_ASSERT_EQUAL(static_cast<int>(OtaBootAction::Count), static_cast<int>(ota_on_boot(s)));
    TEST_ASSERT_EQUAL(1, s.boots);
    TEST_ASSERT_EQUAL(static_cast<int>(OtaBootAction::Count), static_cast<int>(ota_on_boot(s)));
    TEST_ASSERT_EQUAL(2, s.boots);
    TEST_ASSERT_EQUAL(static_cast<int>(OtaBootAction::Rollback), static_cast<int>(ota_on_boot(s)));
    TEST_ASSERT_FALSE(s.pending);  // the restored image is not pending
    TEST_ASSERT_EQUAL(0, s.boots);
    TEST_ASSERT_EQUAL(static_cast<int>(OtaBootAction::None), static_cast<int>(ota_on_boot(s)));
}

static void test_confirm_stops_counting(void)
{
    OtaBootState s;
    ota_mark_applied(s);
    ota_on_boot(s);
    ota_on_boot(s);
    ota_confirm(s);
    TEST_ASSERT_EQUAL(static_cast<int>(OtaBootAction::None), static_cast<int>(ota_on_boot(s)));
    TEST_ASSERT_EQUAL(static_cast<int>(OtaBootAction::None), static_cast<int>(ota_on_boot(s)));
}

static void test_sha256_hex(void)
{
    uint8_t d[32];
    const std::string h = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    TEST_ASSERT_TRUE(sha256_hex_parse(h, d));
    TEST_ASSERT_EQUAL_STRING(h.c_str(), sha256_hex(d).c_str());
    TEST_ASSERT_FALSE(sha256_hex_parse(h.substr(1), d));                       // 63 digits
    TEST_ASSERT_FALSE(sha256_hex_parse("E" + h.substr(1), d));                 // uppercase
    TEST_ASSERT_FALSE(sha256_hex_parse(h.substr(0, 63) + "g", d));             // not hex
}

static std::string digest_hex(const std::string &msg, size_t chunk)
{
    Sha256Stream s;
    sha256_init(&s);
    for (size_t i = 0; i < msg.size(); i += chunk) {
        const size_t n = msg.size() - i < chunk ? msg.size() - i : chunk;
        sha256_update(&s, reinterpret_cast<const uint8_t *>(msg.data()) + i, n);
    }
    uint8_t d[32];
    sha256_final(&s, d);
    return sha256_hex(d);
}

static void test_sha256_known_answers(void)
{
    // FIPS 180-2 examples, fed whole and in awkward chunk sizes.
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                             digest_hex("", 1).c_str());
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                             digest_hex("abc", 1).c_str());
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    for (size_t chunk : {1u, 7u, 55u, 64u, 100u}) {
        TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                                 digest_hex(two, chunk).c_str());
    }
    const std::string million(1000000, 'a');
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                             digest_hex(million, 4092).c_str());
}

static void test_image_header(void)
{
    const uint8_t good[] = {0xE9, 0x03};
    const uint8_t bad[] = {0x7F, 'E', 'L', 'F'};
    TEST_ASSERT_TRUE(ota_image_header_ok(good, sizeof(good)));
    TEST_ASSERT_FALSE(ota_image_header_ok(bad, sizeof(bad)));
    TEST_ASSERT_FALSE(ota_image_header_ok(good, 0));
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_pending_does_nothing);
    RUN_TEST(test_rollback_on_third_unconfirmed_boot);
    RUN_TEST(test_confirm_stops_counting);
    RUN_TEST(test_sha256_hex);
    RUN_TEST(test_sha256_known_answers);
    RUN_TEST(test_image_header);
    return UNITY_END();
}
