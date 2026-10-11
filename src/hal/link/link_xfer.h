/**
 * @file      link_xfer.h
 * @brief     PagerLink v1 bulk-transfer engine (PROTOCOL.md §4) + CRC-32.
 *
 * Pure C++17, no Arduino / ESP-IDF headers, so it is unit-tested on the host.
 * One XferSender or XferReceiver per transfer; the owner (handlers_fs.cpp on
 * the pager) feeds it the DATA / ACK payloads routed by Rpc::on_bulk() and
 * supplies the storage and send callbacks. Not thread-safe: like Rpc, every
 * call runs on the link service task.
 *
 * Flow control:
 *  - Credits count DATA messages. The sender keeps at most `credits` DATA
 *    messages past the last acknowledged offset; the implicit initial grant
 *    is INITIAL_CREDITS (link_frame.h).
 *  - ACK.next acknowledges every byte below it and its credits replace the
 *    sender's window, counting from that offset.
 *  - The receiver only ACKs after DATA arrives: every `ack_every` messages,
 *    on completion, and immediately (a re-ACK of its current offset) when a
 *    DATA offset is not the one it expects. A sender that gets an ACK with no
 *    progress while it has bytes in flight therefore rewinds to that offset
 *    (go-back-N) and resends from there.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace pagerlink {

/** Largest DATA chunk: MAX_PAYLOAD minus the u32 offset. */
constexpr size_t MAX_DATA_CHUNK = 4092;

/**
 * CRC-32/ISO-HDLC (zlib.crc32, Go crc32.ChecksumIEEE), incremental:
 * crc32_update(crc32_update(0, a), b) == CRC of a followed by b.
 * Start a new checksum with crc = 0.
 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t n);

enum class XferState : uint8_t { Active, Done, Failed };

/** Sends `size` bytes as DATA messages under the receiver's credit window. */
class XferSender {
public:
    /** Fill `buf` with exactly `n` bytes of the source at `offset`; false fails the transfer. */
    using ReadFn = std::function<bool(uint32_t offset, uint8_t *buf, size_t n)>;
    /** Send one encoded DATA payload (u32 offset + bytes); false fails the transfer. */
    using SendFn = std::function<bool(const std::string &data_payload)>;

    /**
     * `chunk` is the DATA size (clamped to 1..MAX_DATA_CHUNK); every message
     * but the last carries exactly `chunk` bytes. `start` (≤ size) is the
     * first offset to send — a resumed read. A zero-length remainder is Done
     * immediately.
     */
    XferSender(uint32_t size, size_t chunk, ReadFn read, SendFn send, uint32_t start = 0);

    /** Send DATA while the window allows. Returns the state afterwards. */
    XferState pump();
    /**
     * Feed one ACK payload. A malformed payload, an ACK below the current
     * acknowledged offset, or one beyond what was sent is ignored. Does not
     * send; call pump() next.
     */
    XferState on_ack(const uint8_t *payload, size_t len);

    XferState state() const { return state_; }
    uint32_t acked() const { return acked_; }
    uint32_t sent() const { return next_; }
    uint32_t size() const { return size_; }

private:
    uint32_t size_;
    size_t chunk_;
    ReadFn read_;
    SendFn send_;
    uint32_t acked_;
    uint32_t next_;
    uint16_t credits_;
    XferState state_;
};

/** Receives `size` bytes of DATA in order and acknowledges them. */
class XferReceiver {
public:
    /** Persist `n` bytes at `offset` (always the next expected offset); false fails the transfer. */
    using WriteFn = std::function<bool(uint32_t offset, const uint8_t *bytes, size_t n)>;
    /** Send one encoded ACK payload (u32 next + u16 credits); false fails the transfer. */
    using AckFn = std::function<bool(const std::string &ack_payload)>;

    /**
     * `window` is the credit grant sent in every ACK, `ack_every` (≥ 1) how
     * many in-order DATA messages arrive between ACKs. `start` is the first
     * expected offset. A zero-length transfer is Done immediately.
     */
    XferReceiver(uint32_t size, WriteFn write, AckFn ack, uint16_t window = 16,
                 unsigned ack_every = 4, uint32_t start = 0);

    /**
     * Feed one DATA payload. Malformed → ignored. Wrong offset → re-ACK of the
     * current offset, bytes dropped. Bytes past `size` or a failed write /
     * ACK → Failed. Returns the state afterwards.
     */
    XferState on_data(const uint8_t *payload, size_t len);

    XferState state() const { return state_; }
    uint32_t received() const { return next_; }
    uint32_t size() const { return size_; }

private:
    bool send_ack();

    uint32_t size_;
    WriteFn write_;
    AckFn ack_;
    uint16_t window_;
    unsigned ack_every_;
    unsigned since_ack_;
    uint32_t next_;
    XferState state_;
};

}  // namespace pagerlink
