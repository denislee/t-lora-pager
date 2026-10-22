/**
 * @file      link_frame.cpp
 * @brief     PagerLink v1 framing codec implementation.
 *
 * Deliberately dependency-free (standard library only) and a line-by-line
 * mirror of the reference Reassembler/segmenter in protocol/tools/mkvectors.py,
 * so the golden vectors cannot drift from the firmware.
 */
#include "hal/link/link_frame.h"

#include <algorithm>
#include <utility>

namespace pagerlink {

namespace {

inline uint16_t read_u16_le(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t read_u32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void append_u16_le(std::string &out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

inline void append_u32_le(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

}  // namespace

const char *decode_error_name(DecodeError e) {
    switch (e) {
        case DecodeError::None: return "none";
        case DecodeError::Short: return "short";
        case DecodeError::Version: return "version";
        case DecodeError::Type: return "type";
        case DecodeError::Oversize: return "oversize";
        case DecodeError::Length: return "length";
    }
    return "none";
}

const char *reset_reason_name(ResetReason r) {
    switch (r) {
        case ResetReason::Empty: return "empty";
        case ResetReason::SeqGap: return "seq_gap";
        case ResetReason::UnexpectedStart: return "unexpected_start";
        case ResetReason::UnexpectedContinue: return "unexpected_continue";
        case ResetReason::Oversize: return "oversize";
    }
    return "empty";
}

bool encode_message(MsgType t, uint16_t id, const uint8_t *payload, size_t len, std::string &out) {
    if (len > MAX_PAYLOAD) {
        return false;
    }
    std::string buf;
    buf.reserve(HEADER_SIZE + len);
    buf.push_back(static_cast<char>(PROTO_VERSION));
    buf.push_back(static_cast<char>(static_cast<uint8_t>(t)));
    append_u16_le(buf, id);
    append_u16_le(buf, static_cast<uint16_t>(len));
    if (len != 0) {
        buf.append(reinterpret_cast<const char *>(payload), len);
    }
    out = std::move(buf);
    return true;
}

DecodeError decode_message(const uint8_t *buf, size_t len, Message &out) {
    if (len < HEADER_SIZE) {
        return DecodeError::Short;
    }
    const uint8_t ver = buf[0];
    const uint8_t type = buf[1];
    const uint16_t id = read_u16_le(buf + 2);
    const uint16_t plen = read_u16_le(buf + 4);
    if (ver != PROTO_VERSION) {
        return DecodeError::Version;
    }
    if (type > 5) {
        return DecodeError::Type;
    }
    if (plen > MAX_PAYLOAD) {
        return DecodeError::Oversize;
    }
    if (HEADER_SIZE + static_cast<size_t>(plen) != len) {
        return DecodeError::Length;
    }
    out.type = static_cast<MsgType>(type);
    out.id = id;
    out.payload.assign(reinterpret_cast<const char *>(buf) + HEADER_SIZE, plen);
    return DecodeError::None;
}

void Segmenter::reset() { seq_ = 0; }

uint8_t Segmenter::seq() const { return seq_; }

void Segmenter::set_seq(uint8_t s) { seq_ = s & SEQ_MASK; }

bool Segmenter::send(const uint8_t *msg, size_t len, size_t packet_size, const Sink &sink) {
    if (packet_size < 2 || len == 0) {
        return false;
    }
    const size_t room = packet_size - 1;
    for (size_t off = 0; off < len; off += room) {
        const size_t take = std::min(room, len - off);
        uint8_t flags = seq_ & SEQ_MASK;
        if (off == 0) {
            flags |= FLAG_START;
        }
        if (off + room >= len) {
            flags |= FLAG_END;
        }
        std::string pkt;
        pkt.reserve(1 + take);
        pkt.push_back(static_cast<char>(flags));
        pkt.append(reinterpret_cast<const char *>(msg) + off, take);
        if (!sink(reinterpret_cast<const uint8_t *>(pkt.data()), pkt.size())) {
            return false;
        }
        seq_ = (seq_ + 1) & SEQ_MASK;
    }
    return true;
}

Reassembler::Reassembler(MessageFn on_message, ResetFn on_reset)
    : on_message_(std::move(on_message)), on_reset_(std::move(on_reset)) {}

void Reassembler::reset() {
    expected_ = 0;
    partial_.clear();
    open_ = false;
    discarding_ = false;
}

void Reassembler::feed(const uint8_t *pkt, size_t len) {
    if (len < 2) {
        on_reset_(ResetReason::Empty);
        return;
    }
    const uint8_t flags = pkt[0];
    const uint8_t *data = pkt + 1;
    const size_t dlen = len - 1;
    const uint8_t seq = flags & SEQ_MASK;

    if (seq != expected_) {
        on_reset_(ResetReason::SeqGap);
        partial_.clear();
        open_ = false;
        discarding_ = true;
    }
    expected_ = (seq + 1) & SEQ_MASK;

    if (flags & FLAG_START) {
        discarding_ = false;
        if (open_) {
            on_reset_(ResetReason::UnexpectedStart);
        }
        partial_.clear();
        open_ = true;
    } else if (!open_) {
        if (!discarding_) {
            on_reset_(ResetReason::UnexpectedContinue);
        }
        return;
    }

    if (partial_.size() + dlen > MAX_MESSAGE) {
        on_reset_(ResetReason::Oversize);
        partial_.clear();
        open_ = false;
        discarding_ = true;
        return;
    }
    partial_.append(reinterpret_cast<const char *>(data), dlen);
    if (flags & FLAG_END) {
        on_message_(reinterpret_cast<const uint8_t *>(partial_.data()), partial_.size());
        partial_.clear();
        open_ = false;
    }
}

bool encode_data(uint32_t offset, const uint8_t *bytes, size_t n, std::string &out) {
    if (n > MAX_PAYLOAD - 4) {
        return false;
    }
    std::string buf;
    buf.reserve(4 + n);
    append_u32_le(buf, offset);
    if (n != 0) {
        buf.append(reinterpret_cast<const char *>(bytes), n);
    }
    out = std::move(buf);
    return true;
}

bool decode_data(const uint8_t *payload, size_t len, uint32_t &offset, const uint8_t *&bytes,
                 size_t &n) {
    if (len < 4) {
        return false;
    }
    offset = read_u32_le(payload);
    bytes = payload + 4;
    n = len - 4;
    return true;
}

void encode_ack(uint32_t next, uint16_t credits, std::string &out) {
    std::string buf;
    buf.reserve(6);
    append_u32_le(buf, next);
    append_u16_le(buf, credits);
    out = std::move(buf);
}

bool decode_ack(const uint8_t *payload, size_t len, uint32_t &next, uint16_t &credits) {
    if (len != 6) {
        return false;
    }
    next = read_u32_le(payload);
    credits = read_u16_le(payload + 4);
    return true;
}

uint16_t rx_credits_for(size_t queue_packets, size_t packet_size) {
    if (packet_size < 2) {
        return 1;
    }
    if (queue_packets == 0) {
        return INITIAL_CREDITS;
    }
    const size_t per_message = packets_per_message(MAX_MESSAGE, packet_size);
    const size_t fit = per_message == 0 ? 0 : queue_packets / per_message;
    if (fit < 1) {
        return 1;
    }
    if (fit > INITIAL_CREDITS) {
        return INITIAL_CREDITS;
    }
    return static_cast<uint16_t>(fit);
}

unsigned rx_ack_every_for(uint16_t credits) {
    if (credits >= 8) {
        return 4;
    }
    if (credits >= 4) {
        return 2;
    }
    return 1;
}

size_t packet_size_for_mtu(uint16_t mtu, uint16_t max_mtu) {
    if (mtu > max_mtu) {
        mtu = max_mtu;
    }
    if (mtu < 23) {
        mtu = 23;
    }
    return static_cast<size_t>(mtu) - 3;
}

ConnBand next_conn_band(ConnBand cur, bool activity, uint32_t now_ms, uint32_t last_activity_ms) {
    if (activity) {
        return ConnBand::Transfer;
    }
    if (cur == ConnBand::Transfer &&
        static_cast<uint32_t>(now_ms - last_activity_ms) >= BULK_IDLE_MS) {
        return ConnBand::Idle;
    }
    return cur;
}

uint32_t min_wait_ms(uint32_t best, uint32_t now_ms, uint32_t due_ms) {
    const int32_t left = static_cast<int32_t>(due_ms - now_ms);
    const uint32_t wait = left > 0 ? static_cast<uint32_t>(left) : 0;
    return wait < best ? wait : best;
}

}  // namespace pagerlink
