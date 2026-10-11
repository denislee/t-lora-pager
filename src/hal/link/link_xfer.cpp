/**
 * @file      link_xfer.cpp
 * @brief     PagerLink v1 bulk-transfer engine (PROTOCOL.md §4) + CRC-32.
 *
 * Standard library only; mirrors PROTOCOL.md §4. Credits count DATA messages
 * and every ACK replaces the sender's window wholesale, counted from the
 * acknowledged offset. An ACK that makes no progress while bytes are in flight
 * rewinds the sender to that offset (go-back-N), so a DATA message lost on the
 * link is retransmitted once the receiver re-ACKs the offset it is waiting for.
 */
#include "hal/link/link_xfer.h"

#include <algorithm>
#include <utility>

#include "hal/link/link_frame.h"

namespace pagerlink {

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t n) {
    /* Reflected CRC-32/ISO-HDLC table, built once on the first call. */
    static uint32_t table[256];
    static bool table_ready = false;
    if (!table_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = true;
    }

    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

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
    /* Below what we already know, or beyond what we have sent: ignore. */
    if (next < acked_ || next > next_) {
        return state_;
    }
    /* No progress while bytes are in flight: rewind and resend (go-back-N). */
    if (next == acked_ && next_ > acked_) {
        next_ = acked_;
    }
    acked_ = next;
    credits_ = credits;
    if (acked_ == size_) {
        state_ = XferState::Done;
    }
    return state_;
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
      state_(start >= size ? XferState::Done : XferState::Active) {}

bool XferReceiver::send_ack() {
    std::string payload;
    encode_ack(next_, window_, payload);
    return ack_(payload);
}

XferState XferReceiver::on_data(const uint8_t *payload, size_t len) {
    if (state_ != XferState::Active) {
        return state_;
    }

    uint32_t offset = 0;
    const uint8_t *bytes = nullptr;
    size_t n = 0;
    if (!decode_data(payload, len, offset, bytes, n)) {
        return state_; /* malformed: ignore */
    }

    /* Not the offset we are waiting for: re-ACK it and drop the bytes. */
    if (offset != next_) {
        if (!send_ack()) {
            state_ = XferState::Failed;
        }
        return state_;
    }
    if (n > static_cast<size_t>(size_ - next_)) {
        state_ = XferState::Failed;
        return state_;
    }
    if (n > 0 && !write_(offset, bytes, n)) {
        state_ = XferState::Failed;
        return state_;
    }
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
