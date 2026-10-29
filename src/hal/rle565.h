/**
 * @file      rle565.h
 * @brief     The `rgb565-rle` screen-capture encoding (PROTOCOL.md §6.6).
 *
 * Pure C++ (C++11 on the device, gnu++17 in native_test). A stream of 4-byte
 * runs, `u16 count, u16 pixel`, little-endian, row-major; counts are 1..65535,
 * runs may cross rows and add up to exactly w*h. Pinned by
 * protocol/vectors/screen.json (test/test_rle565); PagerKit and the Go tool
 * decode the same vectors.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hal {

/** Encode `n` RGB565 pixels, appending the runs to `out`. */
void rle565_encode(const uint16_t *px, size_t n, std::string &out);

/**
 * Decode a stream for a `w` x `h` image into `out` (w*h pixels). Returns false
 * with `why` set when the length is not a multiple of 4, a count is 0, or the
 * counts do not add up to exactly w*h.
 */
bool rle565_decode(const uint8_t *data, size_t len, uint32_t w, uint32_t h,
                   std::vector<uint16_t> &out, std::string &why);

}  // namespace hal
