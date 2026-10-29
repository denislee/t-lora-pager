/**
 * @file      rle565.cpp
 * @brief     `rgb565-rle` encode / decode — see rle565.h.
 */
#include "rle565.h"

namespace hal {

namespace {

void put_run(std::string &out, uint16_t count, uint16_t px) {
    out.push_back(static_cast<char>(count & 0xFF));
    out.push_back(static_cast<char>(count >> 8));
    out.push_back(static_cast<char>(px & 0xFF));
    out.push_back(static_cast<char>(px >> 8));
}

}  // namespace

void rle565_encode(const uint16_t *px, size_t n, std::string &out) {
    size_t i = 0;
    while (i < n) {
        const uint16_t v = px[i];
        size_t run = 1;
        while (i + run < n && px[i + run] == v && run < 0xFFFF) run++;
        put_run(out, static_cast<uint16_t>(run), v);
        i += run;
    }
}

bool rle565_decode(const uint8_t *data, size_t len, uint32_t w, uint32_t h,
                   std::vector<uint16_t> &out, std::string &why) {
    out.clear();
    if (len % 4 != 0) {
        why = "length not a multiple of 4";
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(w) * h;
    out.reserve(static_cast<size_t>(total));
    for (size_t i = 0; i < len; i += 4) {
        const uint16_t count = static_cast<uint16_t>(data[i] | (data[i + 1] << 8));
        const uint16_t px = static_cast<uint16_t>(data[i + 2] | (data[i + 3] << 8));
        if (count == 0) {
            why = "zero-length run";
            return false;
        }
        if (out.size() + count > total) {
            why = "more pixels than w*h";
            return false;
        }
        out.insert(out.end(), count, px);
    }
    if (out.size() != total) {
        why = "fewer pixels than w*h";
        return false;
    }
    return true;
}

}  // namespace hal
