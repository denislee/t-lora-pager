/**
 * @file      nmea_rmc.h
 * @brief     NMEA RMC date+time gate for the GPS clock sync.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace hal {

// One-sentence RMC clock source. TinyGPSPlus is deliberately not used here:
// it commits date and time from RMC sentences regardless of the status field,
// and it reads a module's power-on default 1980-01-06 (yy=80) as 2080. This
// gate instead takes date and time atomically from one checksum-valid RMC
// sentence with status A, so a stale or default date can never pair with a
// live time.
class NmeaRmcClock {
public:
    // Discards any partially-received sentence and the latched parser state.
    void reset();

    // Feeds one UART byte. Returns true and writes *utc (Unix seconds, UTC)
    // only when this byte completed an acceptable sentence; *utc is never
    // touched otherwise.
    bool feed(char c, int64_t *utc);

private:
    char buf_[96];
    size_t len_ = 0;
    bool in_sentence_ = false;
};

} // namespace hal
