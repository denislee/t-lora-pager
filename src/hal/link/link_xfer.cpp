/**
 * @file      link_xfer.cpp
 * @brief     PagerLink v1 bulk-transfer engine (PROTOCOL.md §4) + CRC-32.
 *
 * Standard library only; mirrors PROTOCOL.md §4. Credits count DATA messages
 * and every ACK replaces the sender's window wholesale, counted from the
 * acknowledged offset. The first ACK that makes no progress while bytes are in
 * flight rewinds the sender to that offset (one rewind per acknowledged
 * offset, go-back-N); the receiver sends at most two ACKs per gap — one that
 * tells the sender the offset if it has not seen it, then the duplicate that
 * triggers its one rewind — and re-ACKs its final offset if the completion ACK
 * is lost. A lost DATA with no later message to trigger a duplicate ACK is
 * recovered by the owner's retransmit timeout: it calls
 * XferSender::retransmit() and then pump().
 */
#include "hal/link/link_xfer.h"

#include <algorithm>
#include <utility>

#include "hal/link/link_frame.h"

#ifdef ARDUINO
#include <esp_rom_crc.h>
#endif

namespace pagerlink {

#ifdef ARDUINO

/* ROM CRC-32 (esp_rom_crc.h): reflected poly 0xEDB88320 with a '~' on the way
 * in and on the way out, i.e. exactly crc32_update()'s contract: seed 0 gives
 * CRC-32/ISO-HDLC, and feeding a previous result back in continues it. */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t n) {
    if (n == 0) return crc;
    return esp_rom_crc32_le(crc, data, static_cast<uint32_t>(n));
}

#else

namespace {

/* Reflected CRC-32/ISO-HDLC table, built at compile time into rodata: no
 * lazy init, so no first-call race between link_task and a worker. */
struct Crc32Table {
    uint32_t v[256];
};

constexpr Crc32Table make_crc32_table() {
    Crc32Table t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        t.v[i] = c;
    }
    return t;
}

constexpr Crc32Table CRC32_TABLE = make_crc32_table();
static_assert(CRC32_TABLE.v[1] == 0x77073096u, "CRC-32 table");
static_assert(CRC32_TABLE.v[255] == 0x2D02EF8Du, "CRC-32 table");

}  // namespace

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc = CRC32_TABLE.v[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

#endif

// ---------------------------------------------------------------------------
// XferSender
// ---------------------------------------------------------------------------

XferSender::XferSender(uint32_t size, size_t chunk, ReadFn read, SendFn send, uint32_t start)
    : size_(size),
      chunk_(chunk < 1 ? 1 : (chunk > MAX_DATA_CHUNK ? MAX_DATA_CHUNK : chunk)),
      read_(std::move(read)),
      send_(std::move(send)),
      acked_(start > size ? size : start),
      next_(acked_),
      high_(acked_),
      rewound_(false),
      credits_(INITIAL_CREDITS),
      state_(acked_ >= size ? XferState::Done : XferState::Active) {}

XferState XferSender::pump() {
    if (state_ != XferState::Active) {
        return state_;
    }

    while (next_ < size_) {
        /* In-flight DATA messages past the acknowledged offset. */
        const uint32_t inflight =
            (next_ - acked_ + static_cast<uint32_t>(chunk_) - 1) / static_cast<uint32_t>(chunk_);
        if (inflight >= static_cast<uint32_t>(credits_)) {
            break;
        }

        const size_t n = std::min(chunk_, static_cast<size_t>(size_ - next_));
        std::string buf(n, '\0');
        if (!read_(next_, reinterpret_cast<uint8_t *>(&buf[0]), n)) {
            state_ = XferState::Failed;
            return state_;
        }

        std::string payload;
        if (!encode_data(next_, reinterpret_cast<const uint8_t *>(buf.data()), n, payload)) {
            state_ = XferState::Failed;
            return state_;
        }
        if (!send_(payload)) {
            state_ = XferState::Failed;
            return state_;
        }
        next_ += static_cast<uint32_t>(n);
        if (next_ > high_) {
            high_ = next_;
        }
    }
    return state_;
}

XferState XferSender::on_ack(const uint8_t *payload, size_t len) {
    if (state_ != XferState::Active) {
        return state_;
    }

    uint32_t next = 0;
    uint16_t credits = 0;
    if (!decode_ack(payload, len, next, credits)) {
        return state_;
    }
    /* Below what we already know, or beyond what we ever sent: ignore. */
    if (next < acked_ || next > high_) {
        return state_;
    }
    if (next == acked_) {
        /* No progress while bytes are in flight: one go-back-N rewind per
         * acknowledged offset. Later duplicates only replace credits. */
        if (next_ > acked_ && !rewound_) {
            next_ = acked_;
            rewound_ = true;
        }
    } else {
        /* Progress: an ACK that overtook a rewind moves the send point on. */
        rewound_ = false;
        if (next_ < next) {
            next_ = next;
        }
    }
    acked_ = next;
    credits_ = credits;
    if (acked_ == size_) {
        state_ = XferState::Done;
    }
    return state_;
}

bool XferSender::retransmit() {
    if (state_ != XferState::Active || high_ == acked_) {
        return false;
    }
    next_ = acked_;
    rewound_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// XferReceiver
// ---------------------------------------------------------------------------

XferReceiver::XferReceiver(uint32_t size, WriteFn write, AckFn ack, uint16_t window,
                           unsigned ack_every, uint32_t start)
    : size_(size),
      write_(std::move(write)),
      ack_(std::move(ack)),
      window_(window),
      ack_every_(ack_every < 1 ? 1 : ack_every),
      since_ack_(0),
      next_(start),
      last_ack_(start),
      gap_acked_(false),
      crc_(0),
      state_(start >= size ? XferState::Done : XferState::Active) {}

bool XferReceiver::send_ack() {
    std::string payload;
    encode_ack(next_, window_, payload);
    if (!ack_(payload)) {
        return false;
    }
    last_ack_ = next_;
    return true;
}

XferState XferReceiver::on_data(const uint8_t *payload, size_t len) {
    if (state_ == XferState::Done) {
        /* The sender retransmitted: our completion ACK was lost. Re-ACK the
         * final offset (a failed send must not undo the stored bytes). */
        uint32_t offset = 0;
        const uint8_t *bytes = nullptr;
        size_t n = 0;
        if (decode_data(payload, len, offset, bytes, n)) {
            (void)send_ack();
        }
        return state_;
    }
    if (state_ != XferState::Active) {
        return state_;
    }

    uint32_t offset = 0;
    const uint8_t *bytes = nullptr;
    size_t n = 0;
    if (!decode_data(payload, len, offset, bytes, n)) {
        return state_; /* malformed: ignore */
    }

    /* Not the offset we are waiting for: at most two ACKs per gap, then drop
     * the bytes. The first — only when the last ACK we sent was below `next_`
     * — is progress for the sender, which learns the new offset; the second is
     * the duplicate that triggers its one rewind. Further DATA of the same gap
     * would only storm the link. */
    if (offset != next_) {
        if (!gap_acked_) {
            gap_acked_ = true;
            if (last_ack_ != next_ && !send_ack()) {
                state_ = XferState::Failed;
                return state_;
            }
            if (!send_ack()) {
                state_ = XferState::Failed;
            }
        }
        return state_;
    }
    gap_acked_ = false;
    if (n > static_cast<size_t>(size_ - next_)) {
        state_ = XferState::Failed;
        return state_;
    }
    if (n > 0 && !write_(offset, bytes, n)) {
        state_ = XferState::Failed;
        return state_;
    }
    crc_ = crc32_update(crc_, bytes, n);
    next_ += static_cast<uint32_t>(n);
    ++since_ack_;

    if (next_ == size_) {
        state_ = XferState::Done;
        if (!send_ack()) {
            state_ = XferState::Failed;
        }
        return state_;
    }
    if (since_ack_ >= ack_every_) {
        since_ack_ = 0;
        if (!send_ack()) {
            state_ = XferState::Failed;
        }
    }
    return state_;
}

}  // namespace pagerlink
