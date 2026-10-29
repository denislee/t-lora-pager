/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/rle565.{h,cpp} (PROTOCOL.md §6.6).
 *
 * Every image of protocol/vectors/screen.json encodes to exactly its
 * `rle_hex` and decodes back to its pixels; every malformed stream is refused.
 */
#include <unity.h>

#include <string>
#include <vector>

#include "hal/link/link_json.h"
#include "hal/rle565.h"
#include "screen_vectors_gen.h" /* kScreenVectorsJson: protocol/vectors/screen.json */

using pagerlink::JsonObject;

void setUp(void) {}
void tearDown(void) {}

static std::vector<uint8_t> unhex(const std::string &h)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    }
    return out;
}

static std::vector<std::string> array_of(const JsonObject &o, const char *key)
{
    std::string raw;
    std::vector<std::string> items;
    TEST_ASSERT_TRUE(o.get_raw(key, raw));
    TEST_ASSERT_TRUE(pagerlink::json_array_items(raw, items));
    return items;
}

static void test_images(void)
{
    JsonObject r;
    TEST_ASSERT_TRUE(r.parse(kScreenVectorsJson));
    const std::vector<std::string> images = array_of(r, "images");
    TEST_ASSERT_TRUE(images.size() >= 5);
    for (const std::string &elem : images) {
        JsonObject im;
        TEST_ASSERT_TRUE(im.parse(elem));
        std::string name, rle_hex, px_hex;
        int64_t w = 0, h = 0, fill = 0;
        TEST_ASSERT_TRUE(im.get_string("name", name));
        TEST_ASSERT_TRUE(im.get_int("w", w));
        TEST_ASSERT_TRUE(im.get_int("h", h));
        TEST_ASSERT_TRUE(im.get_string("rle_hex", rle_hex));

        std::vector<uint16_t> px;
        if (im.get_int("fill", fill)) {
            px.assign(static_cast<size_t>(w * h), static_cast<uint16_t>(fill));
        } else {
            TEST_ASSERT_TRUE(im.get_string("pixels_hex", px_hex));
            const std::vector<uint8_t> b = unhex(px_hex);
            for (size_t i = 0; i + 1 < b.size(); i += 2) {
                px.push_back(static_cast<uint16_t>(b[i] | (b[i + 1] << 8)));
            }
        }
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<size_t>(w * h), px.size(), name.c_str());

        std::string enc;
        hal::rle565_encode(px.data(), px.size(), enc);
        const std::vector<uint8_t> want = unhex(rle_hex);
        TEST_ASSERT_EQUAL_MESSAGE(want.size(), enc.size(), name.c_str());
        TEST_ASSERT_TRUE_MESSAGE(std::string(want.begin(), want.end()) == enc, name.c_str());

        std::vector<uint16_t> dec;
        std::string why;
        TEST_ASSERT_TRUE_MESSAGE(hal::rle565_decode(want.data(), want.size(),
                                                    static_cast<uint32_t>(w),
                                                    static_cast<uint32_t>(h), dec, why),
                                 name.c_str());
        TEST_ASSERT_TRUE_MESSAGE(dec == px, name.c_str());
    }
}

static void test_malformed(void)
{
    JsonObject r;
    TEST_ASSERT_TRUE(r.parse(kScreenVectorsJson));
    for (const std::string &elem : array_of(r, "malformed")) {
        JsonObject m;
        TEST_ASSERT_TRUE(m.parse(elem));
        std::string name, rle_hex;
        int64_t w = 0, h = 0;
        TEST_ASSERT_TRUE(m.get_string("name", name));
        TEST_ASSERT_TRUE(m.get_int("w", w));
        TEST_ASSERT_TRUE(m.get_int("h", h));
        TEST_ASSERT_TRUE(m.get_string("rle_hex", rle_hex));
        const std::vector<uint8_t> b = unhex(rle_hex);
        std::vector<uint16_t> dec;
        std::string why;
        TEST_ASSERT_FALSE_MESSAGE(hal::rle565_decode(b.data(), b.size(), static_cast<uint32_t>(w),
                                                     static_cast<uint32_t>(h), dec, why),
                                  name.c_str());
        TEST_ASSERT_FALSE_MESSAGE(why.empty(), name.c_str());
    }
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_images);
    RUN_TEST(test_malformed);
    return UNITY_END();
}
