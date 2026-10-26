/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/pbkdf2_sha256.{h,cpp} — the portable
 *            PBKDF2-HMAC-SHA256 used by notes_crypto to derive the
 *            note-encryption key and IV.
 *
 * These vectors pin notes_crypto's derive_key_iv output: NC_ITERATIONS=10000,
 * an 8-byte per-file salt, and a 48-byte key||iv, where key = T_1[0..32) and
 * iv = T_2[0..16). If this implementation drifts, every note already written
 * to disk becomes unreadable — which is exactly what the RFC 7914 vectors and
 * the NC-shaped vectors below are here to catch.
 *
 * Expected values come from Python hashlib.pbkdf2_hmac('sha256', ...) and were
 * cross-checked with `openssl kdf PBKDF2` and `openssl enc -pbkdf2 -P`.
 */
#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hal/pbkdf2_sha256.h"

void setUp(void) {}
void tearDown(void) {}

#define MAX_DK 64

/* ---- helpers ---- */

/* Decode a lowercase hex string into out. Returns the number of bytes written,
 * or 0 if the string is malformed or does not fit. */
static size_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_cap)
{
    if (hex == nullptr) {
        return 0;
    }
    const size_t n = strlen(hex);
    if ((n % 2) != 0 || (n / 2) > out_cap) {
        return 0;
    }
    for (size_t i = 0; i < n / 2; ++i) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return n / 2;
}

/* ---- RFC 7914 §11 vectors ---- */

static const char V1_PW[] = "passwd";
static const char V1_SALT[] = "salt";
static const char V1_EXPECTED[] =
    "55ac046e56e3089fec1691c22544b605"
    "f94185216dde0465e68b9d57c20dacbc"
    "49ca9cccf179b645991664b39d77ef31"
    "7c71b845b1e30bd509112041d3a19783";

static const char V2_PW[] = "Password";
static const char V2_SALT[] = "NaCl";
static const char V2_EXPECTED[] =
    "4ddcd8f60b98be21830cee5ef22701f9"
    "641a4418d04c0414aeff08876b34ab56"
    "a1d425a1225833549adb841b51c9b317"
    "6a272bdebba1d078478f62b397f33c8d";

static void test_rfc7914_c1(void)
{
    uint8_t expected[MAX_DK];
    uint8_t out[MAX_DK];

    TEST_ASSERT_EQUAL_size_t(MAX_DK,
                             hex_to_bytes(V1_EXPECTED, expected, sizeof(expected)));
    TEST_ASSERT_TRUE(hal::pbkdf2_hmac_sha256(
        reinterpret_cast<const uint8_t *>(V1_PW), sizeof(V1_PW) - 1,
        reinterpret_cast<const uint8_t *>(V1_SALT), sizeof(V1_SALT) - 1,
        1, out, MAX_DK));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, MAX_DK);
}

static void test_rfc7914_c80000(void)
{
    uint8_t expected[MAX_DK];
    uint8_t out[MAX_DK];

    TEST_ASSERT_EQUAL_size_t(MAX_DK,
                             hex_to_bytes(V2_EXPECTED, expected, sizeof(expected)));
    TEST_ASSERT_TRUE(hal::pbkdf2_hmac_sha256(
        reinterpret_cast<const uint8_t *>(V2_PW), sizeof(V2_PW) - 1,
        reinterpret_cast<const uint8_t *>(V2_SALT), sizeof(V2_SALT) - 1,
        80000, out, MAX_DK));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, MAX_DK);
}

/* ---- notes_crypto-shaped vectors: c=10000, 8-byte salt, 48-byte key||iv ---- */

struct NcVector {
    const uint8_t *pw;
    size_t pw_len;
    const char *salt_hex;
    uint32_t iterations;
    size_t dk_len;
    const char *expected_hex;
};

/* V3 and V4 passphrases are ASCII literals. */
static const char V3_PW[] = "correct horse battery staple";
static const char V4_PW[] = "hunter2";

/* UTF-8 for "pässwörd-ü" (13 bytes). Split literals so the hex escapes cannot
 * swallow the following ASCII characters. */
static const char V6_PW[] = "p\xc3\xa4" "ssw\xc3\xb6" "rd-\xc3\xbc";

/* 100 bytes of 'x': longer than the 64-byte HMAC block, so the key is
 * pre-hashed. Filled in at the start of test_nc_vectors. */
static uint8_t g_long_pw[100];

static const NcVector NC_VECTORS[] = {
    {reinterpret_cast<const uint8_t *>(V3_PW), sizeof(V3_PW) - 1,
     "0102030405060708", 10000, 48,
     "d3b1ce85988587ff1525f1ba69f8d55a"
     "7ff2243b1c764909f58d4e170def1295"
     "d7da3997d77b01859a0c8b316d4eb945"},
    {reinterpret_cast<const uint8_t *>(V4_PW), sizeof(V4_PW) - 1,
     "a1b2c3d4e5f60718", 10000, 48,
     "eda1ca4a720df3b8a330bac1e567fe4e"
     "27c7dfb2633d2a7ab5f4f8b4edf15665"
     "79130791edefe673199d35ea95e43ba3"},
    {g_long_pw, sizeof(g_long_pw), "0011223344556677", 10000, 48,
     "a61dce4a3a356ea391bfbdd993d769a4"
     "bfa7c01191cec8c99eb9d07e5f9c88f4"
     "c2bc057b73b6cbd6fa63c19f4cf831d9"},
    {reinterpret_cast<const uint8_t *>(V6_PW), sizeof(V6_PW) - 1,
     "fedcba9876543210", 10000, 48,
     "4b3b0a64cfb1d1e6a748362c4f94ccec"
     "b78c8f7b3182dc21f2fff0233154f52d"
     "dde378778bb93f322c8517a7694e2e6a"},
};

static void test_nc_vectors(void)
{
    memset(g_long_pw, 'x', sizeof(g_long_pw));

    for (const NcVector &v : NC_VECTORS) {
        uint8_t salt[64];
        uint8_t expected[MAX_DK];
        uint8_t out[MAX_DK];

        const size_t salt_len = hex_to_bytes(v.salt_hex, salt, sizeof(salt));
        TEST_ASSERT_GREATER_THAN_size_t(0, salt_len);
        TEST_ASSERT_EQUAL_size_t(
            v.dk_len, hex_to_bytes(v.expected_hex, expected, sizeof(expected)));

        memset(out, 0, sizeof(out));
        TEST_ASSERT_TRUE(hal::pbkdf2_hmac_sha256(v.pw, v.pw_len, salt, salt_len,
                                                v.iterations, out, v.dk_len));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, v.dk_len);
    }
}

/* ---- split-block API, as notes_crypto maps key = T1, iv = T2[0..16) ---- */

/* Run one NC-shaped derivation through init + block() and assert the split
 * matches the expected key||iv bytes. */
static void assert_block_split(const uint8_t *pw, size_t pw_len,
                               const char *salt_hex, uint32_t iterations,
                               const char *expected_hex)
{
    uint8_t salt[64];
    uint8_t expected[48];
    uint8_t t1[32];
    uint8_t t2[32];
    hal::Pbkdf2Sha256Ctx ctx;

    const size_t salt_len = hex_to_bytes(salt_hex, salt, sizeof(salt));
    TEST_ASSERT_GREATER_THAN_size_t(0, salt_len);
    TEST_ASSERT_EQUAL_size_t(48, hex_to_bytes(expected_hex, expected, sizeof(expected)));

    hal::pbkdf2_sha256_init(&ctx, pw, pw_len);
    hal::pbkdf2_sha256_block(&ctx, salt, salt_len, iterations, 1, t1);
    hal::pbkdf2_sha256_block(&ctx, salt, salt_len, iterations, 2, t2);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, t1, sizeof(t1));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected + sizeof(t1), t2, 16);
}

static void test_block_api_matches_full(void)
{
    assert_block_split(reinterpret_cast<const uint8_t *>(V3_PW),
                       sizeof(V3_PW) - 1, "0102030405060708", 10000,
                       "d3b1ce85988587ff1525f1ba69f8d55a"
                       "7ff2243b1c764909f58d4e170def1295"
                       "d7da3997d77b01859a0c8b316d4eb945");

    assert_block_split(reinterpret_cast<const uint8_t *>(V4_PW),
                       sizeof(V4_PW) - 1, "a1b2c3d4e5f60718", 10000,
                       "eda1ca4a720df3b8a330bac1e567fe4e"
                       "27c7dfb2633d2a7ab5f4f8b4edf15665"
                       "79130791edefe673199d35ea95e43ba3");

    /* c=1: the two blocks concatenated must give the whole 64-byte RFC output. */
    uint8_t expected[MAX_DK];
    uint8_t blocks[MAX_DK];
    hal::Pbkdf2Sha256Ctx ctx;

    TEST_ASSERT_EQUAL_size_t(MAX_DK,
                             hex_to_bytes(V1_EXPECTED, expected, sizeof(expected)));
    hal::pbkdf2_sha256_init(&ctx, reinterpret_cast<const uint8_t *>(V1_PW),
                            sizeof(V1_PW) - 1);
    hal::pbkdf2_sha256_block(&ctx, reinterpret_cast<const uint8_t *>(V1_SALT),
                             sizeof(V1_SALT) - 1, 1, 1, blocks);
    hal::pbkdf2_sha256_block(&ctx, reinterpret_cast<const uint8_t *>(V1_SALT),
                             sizeof(V1_SALT) - 1, 1, 2, blocks + 32);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, blocks, MAX_DK);
}

static void test_block_order_independent(void)
{
    uint8_t salt[64];
    uint8_t expected[48];
    uint8_t t1[32];
    uint8_t t2[32];
    hal::Pbkdf2Sha256Ctx ctx;

    const size_t salt_len = hex_to_bytes("a1b2c3d4e5f60718", salt, sizeof(salt));
    TEST_ASSERT_GREATER_THAN_size_t(0, salt_len);
    TEST_ASSERT_EQUAL_size_t(
        48, hex_to_bytes("eda1ca4a720df3b8a330bac1e567fe4e"
                         "27c7dfb2633d2a7ab5f4f8b4edf15665"
                         "79130791edefe673199d35ea95e43ba3",
                         expected, sizeof(expected)));

    hal::pbkdf2_sha256_init(&ctx, reinterpret_cast<const uint8_t *>(V4_PW),
                            sizeof(V4_PW) - 1);

    /* Block 2 first: the ctx is read-only, which the dual-core path relies on. */
    hal::pbkdf2_sha256_block(&ctx, salt, salt_len, 10000, 2, t2);
    hal::pbkdf2_sha256_block(&ctx, salt, salt_len, 10000, 1, t1);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, t1, sizeof(t1));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected + sizeof(t1), t2, 16);
}

static void test_truncated_output(void)
{
    uint8_t expected[MAX_DK];
    uint8_t out[MAX_DK];

    TEST_ASSERT_EQUAL_size_t(MAX_DK,
                             hex_to_bytes(V1_EXPECTED, expected, sizeof(expected)));

    memset(out, 0, sizeof(out));
    TEST_ASSERT_TRUE(hal::pbkdf2_hmac_sha256(
        reinterpret_cast<const uint8_t *>(V1_PW), sizeof(V1_PW) - 1,
        reinterpret_cast<const uint8_t *>(V1_SALT), sizeof(V1_SALT) - 1,
        1, out, 20));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 20);

    memset(out, 0, sizeof(out));
    TEST_ASSERT_TRUE(hal::pbkdf2_hmac_sha256(
        reinterpret_cast<const uint8_t *>(V1_PW), sizeof(V1_PW) - 1,
        reinterpret_cast<const uint8_t *>(V1_SALT), sizeof(V1_SALT) - 1,
        1, out, 33));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 33);
}

static void test_invalid_args(void)
{
    uint8_t out[MAX_DK];

    TEST_ASSERT_FALSE(hal::pbkdf2_hmac_sha256(
        reinterpret_cast<const uint8_t *>(V1_PW), sizeof(V1_PW) - 1,
        reinterpret_cast<const uint8_t *>(V1_SALT), sizeof(V1_SALT) - 1,
        0, out, MAX_DK));
    TEST_ASSERT_FALSE(hal::pbkdf2_hmac_sha256(
        reinterpret_cast<const uint8_t *>(V1_PW), sizeof(V1_PW) - 1,
        reinterpret_cast<const uint8_t *>(V1_SALT), sizeof(V1_SALT) - 1,
        1, out, 0));
}

static void test_wipe(void)
{
    uint8_t buf[32];
    memset(buf, 0xAA, sizeof(buf));

    hal::secure_wipe(buf, sizeof(buf));

    for (size_t i = 0; i < sizeof(buf); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x00, buf[i]);
    }
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_rfc7914_c1);
    RUN_TEST(test_rfc7914_c80000);
    RUN_TEST(test_nc_vectors);
    RUN_TEST(test_block_api_matches_full);
    RUN_TEST(test_block_order_independent);
    RUN_TEST(test_truncated_output);
    RUN_TEST(test_invalid_args);
    RUN_TEST(test_wipe);
    return UNITY_END();
}
