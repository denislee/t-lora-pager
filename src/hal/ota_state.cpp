/**
 * @file      ota_state.cpp
 * @brief     OTA rollback bookkeeping and image checks — see ota_state.h.
 */
#include "ota_state.h"

namespace hal {

OtaBootAction ota_on_boot(OtaBootState &s, uint8_t max_boots) {
    if (!s.pending) {
        s.boots = 0;
        return OtaBootAction::None;
    }
    if (s.boots < 0xFF) s.boots++;
    if (s.boots >= max_boots) {
        s = OtaBootState();
        return OtaBootAction::Rollback;
    }
    return OtaBootAction::Count;
}

void ota_confirm(OtaBootState &s) { s = OtaBootState(); }

void ota_mark_applied(OtaBootState &s) {
    s.pending = true;
    s.boots = 0;
}

bool sha256_hex_parse(const std::string &hex, uint8_t out[32]) {
    if (hex.size() != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        uint8_t v = 0;
        for (size_t k = 0; k < 2; k++) {
            const char c = hex[2 * i + k];
            uint8_t nib;
            if (c >= '0' && c <= '9') nib = static_cast<uint8_t>(c - '0');
            else if (c >= 'a' && c <= 'f') nib = static_cast<uint8_t>(c - 'a' + 10);
            else return false;
            v = static_cast<uint8_t>((v << 4) | nib);
        }
        out[i] = v;
    }
    return true;
}

std::string sha256_hex(const uint8_t d[32]) {
    static const char kHex[] = "0123456789abcdef";
    std::string s(64, '0');
    for (size_t i = 0; i < 32; i++) {
        s[2 * i] = kHex[d[i] >> 4];
        s[2 * i + 1] = kHex[d[i] & 0x0F];
    }
    return s;
}

bool ota_image_header_ok(const uint8_t *hdr, size_t len) { return len >= 1 && hdr[0] == 0xE9; }

}  // namespace hal
