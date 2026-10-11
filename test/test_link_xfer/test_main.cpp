/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/link/link_xfer.{h,cpp}.
 *
 * Drives an XferSender against an XferReceiver over a pair of std::deque
 * payload queues (one for DATA, one for ACK) so a test can deliver, drop or
 * reorder messages by hand. A loopback helper carries the whole transfer to
 * completion; targeted tests inspect the raw DATA/ACK payloads instead. Covers
 * the bulk-transfer contract in PROTOCOL.md §4: CRC-32, credit windows,
 * go-back-N rewind on a re-ACK, offset checking, overflow failure, resume from
 * a start offset, ack_every batching and read/write failure.
 */
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "hal/link/link_frame.h"
#include "hal/link/link_xfer.h"

using namespace pagerlink;

void setUp(void) {}
void tearDown(void) {}

/* --- loopback harness ---------------------------------------------------- */

/* Carries encoded DATA payloads one way and encoded ACK payloads the other. */
struct Loop {
    std::deque<std::string> data; /* DATA payloads: sender -> receiver */
    std::deque<std::string> ack;  /* ACK payloads:  receiver -> sender */

    bool send_data(const std::string &payload) {
        data.push_back(payload);
        return true;
    }
    bool send_ack(const std::string &payload) {
        ack.push_back(payload);
        return true;
    }

    void deliver_data(XferReceiver &rx) {
        std::string p = std::move(data.front());
        data.pop_front();
        rx.on_data(reinterpret_cast<const uint8_t *>(p.data()), p.size());
    }
    void deliver_ack(XferSender &tx) {
        std::string p = std::move(ack.front());
        ack.pop_front();
        tx.on_ack(reinterpret_cast<const uint8_t *>(p.data()), p.size());
    }
    void deliver_all_data(XferReceiver &rx) {
        while (!data.empty()) {
            deliver_data(rx);
        }
    }
    void deliver_all_acks(XferSender &tx) {
        while (!ack.empty()) {
            deliver_ack(tx);
        }
    }
};

/* --- helpers ------------------------------------------------------------- */

/* Deterministic source pattern: (i*7+3) & 0xFF. */
static std::vector<uint8_t> make_source(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFFu);
    }
    return v;
}

static void feed_data(XferReceiver &rx, uint32_t offset, const uint8_t *bytes, size_t n) {
    std::string payload;
    TEST_ASSERT_TRUE(encode_data(offset, bytes, n, payload));
    rx.on_data(reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
}

/* --- tests --------------------------------------------------------------- */

static void test_crc32_check_value(void) {
    const char *s = "123456789";
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u,
                             crc32_update(0, reinterpret_cast<const uint8_t *>(s), 9));
    TEST_ASSERT_EQUAL_UINT32(0u, crc32_update(0, nullptr, 0));

    /* An incremental split must equal the one-shot checksum. */
    const uint32_t one = crc32_update(0, reinterpret_cast<const uint8_t *>(s), 9);
    uint32_t part = crc32_update(0, reinterpret_cast<const uint8_t *>(s), 4);
    part = crc32_update(part, reinterpret_cast<const uint8_t *>(s) + 4, 5);
    TEST_ASSERT_EQUAL_UINT32(one, part);
}

static void test_round_trip_lossless(void) {
    const uint32_t size = 100000;
    std::vector<uint8_t> src = make_source(size);
    std::vector<uint8_t> dst(size, 0);

    Loop loop;
    XferSender tx(
        size, 4092,
        [&](uint32_t off, uint8_t *buf, size_t n) {
            std::memcpy(buf, src.data() + off, n);
            return true;
        },
        [&](const std::string &p) { return loop.send_data(p); });
    XferReceiver rx(
        size,
        [&](uint32_t off, const uint8_t *b, size_t n) {
            std::memcpy(dst.data() + off, b, n);
            return true;
        },
        [&](const std::string &p) { return loop.send_ack(p); });

    for (int i = 0; i < 1000000 && (tx.state() != XferState::Done || rx.state() != XferState::Done);
         ++i) {
        tx.pump();
        loop.deliver_all_data(rx);
        loop.deliver_all_acks(tx);
    }

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(tx.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(rx.state()));
    TEST_ASSERT_EQUAL_UINT32(size, rx.received());
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(src.data(), dst.data(), size));
}

static void test_sender_respects_credits(void) {
    const uint32_t size = 20 * 4092;
    std::vector<uint8_t> src = make_source(size);

    Loop loop;
    XferSender tx(
        size, 4092,
        [&](uint32_t off, uint8_t *buf, size_t n) {
            std::memcpy(buf, src.data() + off, n);
            return true;
        },
        [&](const std::string &p) { return loop.send_data(p); });

    tx.pump(); /* no ACKs delivered yet */

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(INITIAL_CREDITS),
                             static_cast<uint32_t>(loop.data.size()));
    TEST_ASSERT_EQUAL_UINT32(16u * 4092u, tx.sent());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Active),
                            static_cast<uint8_t>(tx.state()));
}

static void test_ack_replaces_window(void) {
    const uint32_t size = 20 * 4092;
    std::vector<uint8_t> src = make_source(size);

    Loop loop;
    XferSender tx(
        size, 4092,
        [&](uint32_t off, uint8_t *buf, size_t n) {
            std::memcpy(buf, src.data() + off, n);
            return true;
        },
        [&](const std::string &p) { return loop.send_data(p); });

    tx.pump(); /* 16 messages in flight, next_ = 16*4092 */

    /* Shrink the window to 2 starting from 4*4092: 12 messages are still in
     * flight, so the sender must not emit anything more. */
    std::string ack;
    encode_ack(4 * 4092, 2, ack);
    tx.on_ack(reinterpret_cast<const uint8_t *>(ack.data()), ack.size());

    const uint32_t before = tx.sent();
    tx.pump();

    TEST_ASSERT_EQUAL_UINT32(before, tx.sent());
    TEST_ASSERT_EQUAL_UINT32(16u * 4092u, tx.sent());
    TEST_ASSERT_EQUAL_UINT32(16u, static_cast<uint32_t>(loop.data.size()));
    TEST_ASSERT_EQUAL_UINT32(4u * 4092u, tx.acked());
}

static void test_rewind_on_duplicate_ack(void) {
    const uint32_t size = 40 * 4092;
    std::vector<uint8_t> src = make_source(size);
    std::vector<uint8_t> dst(size, 0);

    Loop loop;
    XferSender tx(
        size, 4092,
        [&](uint32_t off, uint8_t *buf, size_t n) {
            std::memcpy(buf, src.data() + off, n);
            return true;
        },
        [&](const std::string &p) { return loop.send_data(p); });
    XferReceiver rx(
        size,
        [&](uint32_t off, const uint8_t *b, size_t n) {
            std::memcpy(dst.data() + off, b, n);
            return true;
        },
        [&](const std::string &p) { return loop.send_ack(p); });

    tx.pump();
    TEST_ASSERT_FALSE(loop.data.empty());

    /* Drop the first DATA message and deliver the rest: every one is at the
     * wrong offset, so the receiver re-ACKs offset 0 without writing. */
    loop.data.pop_front();
    loop.deliver_all_data(rx);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.received());

    /* Feeding the re-ACK rewinds the sender to offset 0. */
    TEST_ASSERT_FALSE(loop.ack.empty());
    loop.deliver_all_acks(tx);
    TEST_ASSERT_EQUAL_UINT32(0u, tx.sent());
    TEST_ASSERT_EQUAL_UINT32(0u, tx.acked());

    /* The transfer now completes losslessly. */
    for (int i = 0; i < 1000000 && (tx.state() != XferState::Done || rx.state() != XferState::Done);
         ++i) {
        tx.pump();
        loop.deliver_all_data(rx);
        loop.deliver_all_acks(tx);
    }

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(tx.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(rx.state()));
    TEST_ASSERT_EQUAL_INT(0, std::memcmp(src.data(), dst.data(), size));
}

static void test_receiver_ignores_wrong_offset(void) {
    const uint32_t size = 3 * 4092;

    Loop loop;
    int writes = 0;
    XferReceiver rx(
        size,
        [&](uint32_t, const uint8_t *, size_t) {
            writes++;
            return true;
        },
        [&](const std::string &p) { return loop.send_ack(p); });

    std::vector<uint8_t> bytes(4092, 0xAB);
    feed_data(rx, 4092, bytes.data(), bytes.size());

    TEST_ASSERT_EQUAL_INT(0, writes);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.received());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Active),
                            static_cast<uint8_t>(rx.state()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(loop.ack.size()));

    uint32_t next = 1;
    uint16_t credits = 0;
    const std::string &a = loop.ack.front();
    TEST_ASSERT_TRUE(
        decode_ack(reinterpret_cast<const uint8_t *>(a.data()), a.size(), next, credits));
    TEST_ASSERT_EQUAL_UINT32(0u, next);
}

static void test_receiver_overflow_fails(void) {
    Loop loop;
    XferReceiver rx(
        10, [](uint32_t, const uint8_t *, size_t) { return true; },
        [&](const std::string &p) { return loop.send_ack(p); });

    std::vector<uint8_t> bytes(11, 0x5A); /* one byte past `size` */
    feed_data(rx, 0, bytes.data(), bytes.size());

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Failed),
                            static_cast<uint8_t>(rx.state()));
    TEST_ASSERT_EQUAL_UINT32(0u, rx.received());
}

static void test_zero_size_done(void) {
    Loop loop;
    int reads = 0;
    int writes = 0;
    XferSender tx(
        0, 4092,
        [&](uint32_t, uint8_t *, size_t) {
            reads++;
            return true;
        },
        [&](const std::string &p) { return loop.send_data(p); });
    XferReceiver rx(
        0,
        [&](uint32_t, const uint8_t *, size_t) {
            writes++;
            return true;
        },
        [&](const std::string &p) { return loop.send_ack(p); });

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(tx.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(rx.state()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(tx.pump()));
    TEST_ASSERT_EQUAL_INT(0, reads);
    TEST_ASSERT_EQUAL_INT(0, writes);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(loop.data.size()));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(loop.ack.size()));
}

static void test_resume_start_offset(void) {
    const uint32_t size = 4 * 4092;
    std::vector<uint8_t> src = make_source(size);
    std::vector<uint8_t> dst(size, 0);

    Loop loop;
    XferSender tx(
        size, 4092,
        [&](uint32_t off, uint8_t *buf, size_t n) {
            std::memcpy(buf, src.data() + off, n);
            return true;
        },
        [&](const std::string &p) { return loop.send_data(p); }, 8184);

    tx.pump();
    TEST_ASSERT_FALSE(loop.data.empty());

    uint32_t off = 0;
    const uint8_t *bytes = nullptr;
    size_t n = 0;
    const std::string &p = loop.data.front();
    TEST_ASSERT_TRUE(
        decode_data(reinterpret_cast<const uint8_t *>(p.data()), p.size(), off, bytes, n));
    TEST_ASSERT_EQUAL_UINT32(8184u, off);

    /* A receiver resuming at the same offset accepts it. */
    XferReceiver rx(
        size,
        [&](uint32_t o, const uint8_t *b, size_t nn) {
            std::memcpy(dst.data() + o, b, nn);
            return true;
        },
        [&](const std::string &q) { return loop.send_ack(q); }, 16, 4, 8184);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Active),
                            static_cast<uint8_t>(rx.state()));
    loop.deliver_data(rx);
    TEST_ASSERT_EQUAL_UINT32(8184u + 4092u, rx.received());
}

static void test_read_failure_fails(void) {
    const uint32_t size = 2 * 4092;
    Loop loop;
    XferSender tx(
        size, 4092, [](uint32_t, uint8_t *, size_t) { return false; },
        [&](const std::string &p) { return loop.send_data(p); });

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Failed),
                            static_cast<uint8_t>(tx.pump()));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(loop.data.size()));
}

static void test_ack_every(void) {
    const uint32_t size = 6 * 4092;
    std::vector<uint8_t> src = make_source(size);

    Loop loop;
    XferReceiver rx(
        size, [](uint32_t, const uint8_t *, size_t) { return true; },
        [&](const std::string &p) { return loop.send_ack(p); }, 16, 4);

    /* The first three in-order messages go unacknowledged. */
    for (uint32_t i = 0; i < 3; ++i) {
        feed_data(rx, i * 4092, src.data() + i * 4092, 4092);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(loop.ack.size()));

    /* The fourth crosses the threshold: exactly one ACK, for 4*4092. */
    feed_data(rx, 3 * 4092, src.data() + 3 * 4092, 4092);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(loop.ack.size()));

    uint32_t next = 0;
    uint16_t credits = 0;
    const std::string &a = loop.ack.front();
    TEST_ASSERT_TRUE(
        decode_ack(reinterpret_cast<const uint8_t *>(a.data()), a.size(), next, credits));
    TEST_ASSERT_EQUAL_UINT32(4u * 4092u, next);

    /* The fifth adds nothing; the sixth completes and ACKs once more. */
    feed_data(rx, 4 * 4092, src.data() + 4 * 4092, 4092);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(loop.ack.size()));

    feed_data(rx, 5 * 4092, src.data() + 5 * 4092, 4092);
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(loop.ack.size()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(rx.state()));

    const std::string &done = loop.ack.back();
    TEST_ASSERT_TRUE(
        decode_ack(reinterpret_cast<const uint8_t *>(done.data()), done.size(), next, credits));
    TEST_ASSERT_EQUAL_UINT32(size, next);
}

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_round_trip_lossless);
    RUN_TEST(test_sender_respects_credits);
    RUN_TEST(test_ack_replaces_window);
    RUN_TEST(test_rewind_on_duplicate_ack);
    RUN_TEST(test_receiver_ignores_wrong_offset);
    RUN_TEST(test_receiver_overflow_fails);
    RUN_TEST(test_zero_size_done);
    RUN_TEST(test_resume_start_offset);
    RUN_TEST(test_read_failure_fails);
    RUN_TEST(test_ack_every);
    return UNITY_END();
}
