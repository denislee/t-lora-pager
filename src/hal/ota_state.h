/**
 * @file      ota_state.h
 * @brief     OTA rollback bookkeeping and image checks (PROTOCOL.md §6.7). Pure C++.
 *
 * hal/ota.cpp persists OtaBootState in NVS and runs ota_on_boot() first thing
 * in setup(); everything that decides something lives here so
 * test/test_ota_state covers it on the host.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hal {

/** Persisted across boots (NVS namespace "ota"). */
struct OtaBootState {
    bool pending = false;  // the running image was applied by ota.apply and has not confirmed
    uint8_t boots = 0;     // boots of the pending image so far, this one included
};

/** Unconfirmed boots after which the previous image is restored. */
constexpr uint8_t kOtaMaxBoots = 3;
/** Uptime after which a pending image confirms itself. */
constexpr uint32_t kOtaConfirmMs = 60000;

enum class OtaBootAction : uint8_t {
    None,      // nothing pending
    Count,     // pending: boot counted, keep running and wait to confirm
    Rollback,  // pending for kOtaMaxBoots boots: switch back to the other slot
};

/** One boot: update `s` and say what to do. Rollback clears `s`. */
OtaBootAction ota_on_boot(OtaBootState &s, uint8_t max_boots = kOtaMaxBoots);

/** The pending image proved itself (kOtaConfirmMs of uptime): clear `s`. */
void ota_confirm(OtaBootState &s);

/** ota.apply succeeded: the next boot runs a pending image. */
void ota_mark_applied(OtaBootState &s);

/** Parse exactly 64 lowercase hex digits. */
bool sha256_hex_parse(const std::string &hex, uint8_t out[32]);

/** Lowercase hex of a 32-byte digest. */
std::string sha256_hex(const uint8_t d[32]);

/** True when `hdr` (at least 1 byte) starts like an ESP32 app image (magic 0xE9). */
bool ota_image_header_ok(const uint8_t *hdr, size_t len);

}  // namespace hal
