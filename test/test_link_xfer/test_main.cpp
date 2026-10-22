/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/link/link_xfer.{h,cpp}.
 *
 * Drives an XferSender against an XferReceiver over a pair of std::deque
 * payload queues (one for DATA, one for ACK) so a test can deliver, drop or
 * reorder messages by hand. A loopback helper carries the whole transfer to
 * completion; targeted tests inspect the raw DATA/ACK payloads instead. A
 * lossy helper adds a drop rule (drop the k-th DATA or ACK send) and simulates
 * the owner's retransmit timer. Covers the bulk-transfer contract in
 * PROTOCOL.md §4: CRC-32, credit windows, one go-back-N rewind per
 * acknowledged offset, at most two ACKs per gap, re-ACK after Done, the
 * owner-driven retransmit timeout, offset checking, overflow failure, resume
 * from a start offset, ack_every batching and read/write failure.
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

/* Lossy loopback: every send is counted, and the k-th (0-based) DATA or ACK
 * send can be discarded without failing the sender/receiver. */
struct LossyLoop {
    std::deque<std::string> data; /* DATA payloads: sender -> receiver */
    std::deque<std::string> ack;  /* ACK payloads:  receiver -> sender */
    unsigned data_sends = 0;
    unsigned ack_sends = 0;
    long drop_data = -1; /* drop this DATA send index, or -1 */
    long drop_ack = -1;  /* drop this ACK send index, or -1 */
    unsigned timers = 0; /* retransmit timer firings */

    bool send_data(const std::string &payload) {
        const unsigned k = data_sends++;
        if (static_cast<long>(k) != drop_data) data.push_back(payload);
        return true; /* a message lost on the link is not a send failure */
    }
    bool send_ack(const std::string &payload) {
        const unsigned k = ack_sends++;
        if (static_cast<long>(k) != drop_ack) ack.push_back(payload);
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

/* Carry a lossy loop to completion. When both queues drain and the sender is
 * not Done, the owner's retransmit timer fires: retransmit() then pump(). */
static void run_lossy(XferSender &tx, XferReceiver &rx, LossyLoop &loop) {
    for (int i = 0; i < 100000; ++i) {
        loop.deliver_all_data(rx);
        loop.deliver_all_acks(tx);
        tx.pump();
        if (tx.state() == XferState::Done) {
            return;
        }
        if (loop.data.empty() && loop.ack.empty()) {
            ++loop.timers;
            tx.retransmit();
            tx.pump();
        }
    }
}

/* Carry a lossy loop to completion one message at a time. The receiver gets a
 * single DATA message per step, then every pending ACK is delivered to the
 * sender with a pump() after each; with both queues empty the owner's
 * retransmit timer fires: retransmit() then pump(). The first window is primed
 * before the loop, so starting the transfer is not a timer firing. */
static void run_lossy_interleaved(XferSender &tx, XferReceiver &rx, LossyLoop &loop) {
    tx.pump();

    for (int i = 0; i < 200000; ++i) {
        if (tx.state() == XferState::Done && rx.state() == XferState::Done) {
            return;
        }
        if (!loop.data.empty()) {
            loop.deliver_data(rx);
            while (!loop.ack.empty()) {
                loop.deliver_ack(tx);
                tx.pump();
            }
        } else if (!loop.ack.empty()) {
            while (!loop.ack.empty()) {
                loop.deliver_ack(tx);
                tx.pump();
            }
        } else if (tx.state() != XferState::Done) {
            ++loop.timers;
            tx.retransmit();
            tx.pump();
        }
    }

    const std::string msg = "interleaved loopback hit the 200000 step bound: data_sends=" +
                            std::to_string(loop.data_sends) +
                            " ack_sends=" + std::to_string(loop.ack_sends) +
                            " timers=" + std::to_string(loop.timers);
    TEST_FAIL_MESSAGE(msg.c_str());
}

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

/* A sender + receiver over a LossyLoop, both fed from a deterministic source. */
struct LossyRig {
    std::vector<uint8_t> src;
    std::vector<uint8_t> dst;
    LossyLoop loop;
    XferSender tx;
    XferReceiver rx;

    explicit LossyRig(uint32_t size)
        : src(make_source(size)),
          dst(size, 0),
          tx(size, 4092,
             [this](uint32_t off, uint8_t *buf, size_t n) {
                 std::memcpy(buf, src.data() + off, n);
                 return true;
             },
             [this](const std::string &p) { return loop.send_data(p); }),
          rx(size,
             [this](uint32_t off, const uint8_t *b, size_t n) {
                 std::memcpy(dst.data() + off, b, n);
                 return true;
             },
             [this](const std::string &p) { return loop.send_ack(p); }) {}

    void run() { run_lossy(tx, rx, loop); }
    void run_interleaved() { run_lossy_interleaved(tx, rx, loop); }

    /* Every byte arrived in order and both ends finished. */
    void check() {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                                static_cast<uint8_t>(tx.state()));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                                static_cast<uint8_t>(rx.state()));
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(src.size()), rx.received());
        TEST_ASSERT_EQUAL_INT(0, std::memcmp(src.data(), dst.data(), src.size()));
    }
};

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

/* Bitwise CRC-32/ISO-HDLC in the esp_rom_crc32_le convention: the caller's
 * seed is complemented on the way in and out, so a nonzero seed composes the
 * same way crc32_update() does. */
static uint32_t crc32_ref(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return ~crc;
}

static void test_crc32_rom_reference(void) {
    /* Empty input: the seed passes through untouched. */
    TEST_ASSERT_EQUAL_UINT32(crc32_ref(0, nullptr, 0), crc32_update(0, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT32(0u, crc32_update(0, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT32(crc32_ref(0x12345678u, nullptr, 0),
                             crc32_update(0x12345678u, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, crc32_update(0x12345678u, nullptr, 0));

    /* Single bytes. */
    const uint8_t singles[] = {0x00, 0xFF, 0x80};
    for (size_t i = 0; i < sizeof(singles); ++i) {
        const uint8_t *b = &singles[i];
        TEST_ASSERT_EQUAL_UINT32(crc32_ref(0, b, 1), crc32_update(0, b, 1));
        TEST_ASSERT_EQUAL_UINT32(crc32_ref(0x12345678u, b, 1),
                                 crc32_update(0x12345678u, b, 1));
    }

    /* Both published check values. */
    const char *digits = "123456789";
    const uint32_t digits_crc =
        crc32_update(0, reinterpret_cast<const uint8_t *>(digits), 9);
    TEST_ASSERT_EQUAL_UINT32(
        crc32_ref(0, reinterpret_cast<const uint8_t *>(digits), 9), digits_crc);
    TEST_ASSERT_EQUAL_UINT32(0xCBF43926u, digits_crc);

    const char *fox = "The quick brown fox jumps over the lazy dog";
    const size_t fox_len = std::strlen(fox);
    const uint32_t fox_crc =
        crc32_update(0, reinterpret_cast<const uint8_t *>(fox), fox_len);
    TEST_ASSERT_EQUAL_UINT32(
        crc32_ref(0, reinterpret_cast<const uint8_t *>(fox), fox_len), fox_crc);
    TEST_ASSERT_EQUAL_UINT32(0x414FA339u, fox_crc);

    /* A longer patterned buffer. */
    std::vector<uint8_t> pattern(4096);
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 31 + 7) & 0xFFu);
    }
    TEST_ASSERT_EQUAL_UINT32(crc32_ref(0, pattern.data(), pattern.size()),
                             crc32_update(0, pattern.data(), pattern.size()));

    /* Every byte value once. */
    std::vector<uint8_t> ramp(256);
    for (size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<uint8_t>(i);
    }
    const uint32_t ramp_crc = crc32_update(0, ramp.data(), ramp.size());
    TEST_ASSERT_EQUAL_UINT32(crc32_ref(0, ramp.data(), ramp.size()), ramp_crc);
    TEST_ASSERT_EQUAL_UINT32(0x29058C73u, ramp_crc);

    /* A nonzero seed continues a checksum exactly like the reference. */
    const char *abc = "abc";
    const char *def = "def";
    const uint32_t seed =
        crc32_update(0, reinterpret_cast<const uint8_t *>(abc), 3);
    TEST_ASSERT_EQUAL_UINT32(
        crc32_ref(0, reinterpret_cast<const uint8_t *>(abc), 3), seed);

    const uint32_t tail =
        crc32_update(seed, reinterpret_cast<const uint8_t *>(def), 3);
    TEST_ASSERT_EQUAL_UINT32(
        crc32_ref(seed, reinterpret_cast<const uint8_t *>(def), 3), tail);

    const char *abcdef = "abcdef";
    TEST_ASSERT_EQUAL_UINT32(
        crc32_update(0, reinterpret_cast<const uint8_t *>(abcdef), 6), tail);
    TEST_ASSERT_EQUAL_UINT32(0x4B8E39EFu, tail);
}

static void test_crc32_incremental_every_split(void) {
    std::vector<uint8_t> buf(1000);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<uint8_t>((i * 131 + 17) & 0xFFu);
    }
    const uint32_t one = crc32_update(0, buf.data(), buf.size());

    /* Every two-way split, including the empty halves at both ends. */
    for (size_t k = 0; k <= buf.size(); ++k) {
        const uint32_t split = crc32_update(
            crc32_update(0, buf.data(), k), buf.data() + k, buf.size() - k);
        TEST_ASSERT_EQUAL_UINT32(one, split);
    }

    /* A three-or-more-way feed with uneven chunk sizes. */
    const size_t sizes[3] = {1, 7, 13};
    uint32_t fed = 0;
    size_t off = 0;
    size_t turn = 0;
    while (off < buf.size()) {
        size_t n = sizes[turn % 3];
        if (n > MAX_DATA_CHUNK) {
            n = MAX_DATA_CHUNK;
        }
        if (n > buf.size() - off) {
            n = buf.size() - off;
        }
        fed = crc32_update(fed, buf.data() + off, n);
        off += n;
        ++turn;
    }
    TEST_ASSERT_EQUAL_UINT32(one, fed);
}

static void test_receiver_crc_matches_payload(void) {
    /* Lossless 100000 B loopback: the receiver's running CRC is the source's. */
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
                            static_cast<uint8_t>(rx.state()));
    TEST_ASSERT_EQUAL_UINT32(crc32_update(0, src.data(), src.size()), rx.crc());

    /* Lossy 102400 B: one mid-stream DATA dropped forces a gap and a rewind, so
     * retransmitted bytes are written again — they must not be counted twice. */
    LossyRig rig(102400);
    rig.loop.drop_data = 5;
    rig.run();
    rig.check();
    TEST_ASSERT_TRUE(rig.loop.data_sends > 26u);
    TEST_ASSERT_EQUAL_UINT32(
        crc32_update(0, rig.src.data(), rig.src.size()), rig.rx.crc());

    /* A zero-length transfer writes nothing, so its CRC is 0. */
    XferReceiver empty(0, [](uint32_t, const uint8_t *, size_t) { return true; },
                       [](const std::string &) { return true; });
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(empty.state()));
    TEST_ASSERT_EQUAL_UINT32(0u, empty.crc());
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
     * wrong offset, but the receiver re-ACKs offset 0 only ONCE per gap. */
    loop.data.pop_front();
    loop.deliver_all_data(rx);
    TEST_ASSERT_EQUAL_UINT32(0u, rx.received());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(loop.ack.size()));

    /* Feeding that re-ACK rewinds the sender to offset 0. */
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

/* --- lossy loopback: one dropped message -------------------------------- */

static void test_lossy_drop_mid_data(void) {
    /* 102400 B = 26 DATA. Drop the 6th message (0-based index 5): it follows
     * the batch ACK at 16368, so the gap's single ACK duplicates that ACK and
     * the sender rewinds without the timer. */
    LossyRig rig(102400);
    rig.loop.drop_data = 5;
    rig.run();
    rig.check();
    TEST_ASSERT_TRUE(rig.loop.data_sends <= 52u);
    TEST_ASSERT_EQUAL_UINT32(0u, rig.loop.timers);
}

static void test_lossy_drop_earlier_mid_data(void) {
    /* Same shape one message earlier (0-based index 4), also recovered by the
     * duplicate ACK rather than the retransmit timer. */
    LossyRig rig(102400);
    rig.loop.drop_data = 4;
    rig.run();
    rig.check();
    TEST_ASSERT_TRUE(rig.loop.data_sends <= 52u);
    TEST_ASSERT_EQUAL_UINT32(0u, rig.loop.timers);
}

static void test_lossy_drop_last_data(void) {
    /* 8192 B = 3 DATA. Losing the last one leaves no later DATA to trigger a
     * duplicate ACK, so only the retransmit timer can recover it. */
    LossyRig rig(8192);
    rig.loop.drop_data = 2;
    rig.run();
    rig.check();
    TEST_ASSERT_EQUAL_UINT32(1u, rig.loop.timers);
}

static void test_lossy_drop_first_data(void) {
    /* 262144 B = 65 DATA, first message lost: the sender's initial window has
     * no progress to report, so the one ACK it earns rewinds it right away. */
    LossyRig rig(262144);
    rig.loop.drop_data = 0;
    rig.run();
    rig.check();
    TEST_ASSERT_TRUE(rig.loop.data_sends <= 130u);
    TEST_ASSERT_TRUE(rig.loop.timers <= 1u);
}

static void test_lossy_drop_final_ack(void) {
    /* 102400 B acks after 4, 8, ... 24 messages and on completion: 7 ACKs,
     * 0-based index 6 is the completion ACK. Losing it leaves the sender one
     * chunk short; the timer resends, and the Done receiver re-ACKs. */
    LossyRig rig(102400);
    rig.loop.drop_ack = 6;
    rig.run();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(rig.rx.state()));
    rig.check();
    TEST_ASSERT_EQUAL_UINT32(1u, rig.loop.timers);
}

static void test_lossy_lossless_counts(void) {
    /* Lossless baseline: 102400 B is 26 DATA messages and never fires the
     * retransmit timer. */
    LossyRig rig(102400);
    rig.run();
    rig.check();
    TEST_ASSERT_EQUAL_UINT32(26u, rig.loop.data_sends);
    TEST_ASSERT_EQUAL_UINT32(0u, rig.loop.timers);
}

/* --- interleaved lossy loopback: one message per step -------------------- */

static void test_interleaved_drop_mid_data(void) {
    /* 102400 B = 26 DATA. Drop the 6th message (0-based index 5): a single
     * message per step opens the gap while later DATA is still in flight, so
     * the duplicate ACK rewinds the sender without the retransmit timer. */
    LossyRig rig(102400);
    rig.loop.drop_data = 5;
    rig.run_interleaved();
    rig.check();
    TEST_ASSERT_TRUE(rig.loop.data_sends <= 52u);
    TEST_ASSERT_EQUAL_UINT32(0u, rig.loop.timers);
}

static void test_interleaved_drop_first_data(void) {
    /* 262144 B = 65 DATA, first message lost: the one ACK the wrong-offset
     * DATA earns rewinds the sender, and one retransmit timer reopens the
     * window; nothing beyond that. */
    LossyRig rig(262144);
    rig.loop.drop_data = 0;
    rig.run_interleaved();
    rig.check();
    TEST_ASSERT_TRUE(rig.loop.data_sends <= 130u);
    TEST_ASSERT_TRUE(rig.loop.timers <= 1u);
}

static void test_interleaved_lossless(void) {
    /* Lossless baseline: 102400 B is still exactly 26 DATA messages and never
     * fires the retransmit timer. */
    LossyRig rig(102400);
    rig.run_interleaved();
    rig.check();
    TEST_ASSERT_EQUAL_UINT32(26u, rig.loop.data_sends);
    TEST_ASSERT_EQUAL_UINT32(0u, rig.loop.timers);
}

/* --- sender / receiver unit contracts ----------------------------------- */

static void test_sender_one_rewind_per_acked(void) {
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

    tx.pump();
    TEST_ASSERT_EQUAL_UINT32(16u * 4092u, tx.sent());

    std::string ack;
    encode_ack(0, INITIAL_CREDITS, ack);
    tx.on_ack(reinterpret_cast<const uint8_t *>(ack.data()), ack.size());

    /* First duplicate: exactly one rewind to the acknowledged offset. */
    TEST_ASSERT_EQUAL_UINT32(0u, tx.acked());
    TEST_ASSERT_EQUAL_UINT32(tx.acked(), tx.sent());

    tx.pump();
    TEST_ASSERT_TRUE(tx.sent() > tx.acked());

    /* Second identical ACK: credits update, the send point does not move. */
    tx.on_ack(reinterpret_cast<const uint8_t *>(ack.data()), ack.size());
    TEST_ASSERT_EQUAL_UINT32(0u, tx.acked());
    TEST_ASSERT_TRUE(tx.sent() > tx.acked());
    TEST_ASSERT_EQUAL_UINT32(16u * 4092u, tx.sent());
}

static void test_receiver_at_most_two_acks_per_gap(void) {
    const uint32_t size = 3 * 4092;
    std::vector<uint8_t> bytes(4092, 0xCD);

    Loop loop;
    XferReceiver rx(
        size, [](uint32_t, const uint8_t *, size_t) { return true; },
        [&](const std::string &p) { return loop.send_ack(p); });

    /* Three wrong-offset DATA messages while next_ == last_ack_ == 0: the
     * first earns exactly one ACK, the duplicate that triggers the rewind. */
    for (int i = 0; i < 3; ++i) {
        feed_data(rx, 2 * 4092, bytes.data(), bytes.size());
    }
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(loop.ack.size()));

    uint32_t next = 0;
    uint16_t credits = 0;
    const std::string &only = loop.ack.front();
    TEST_ASSERT_TRUE(
        decode_ack(reinterpret_cast<const uint8_t *>(only.data()), only.size(), next, credits));
    TEST_ASSERT_EQUAL_UINT32(0u, next);

    /* The expected DATA closes the gap: ack_every=4 keeps this one silent. */
    feed_data(rx, 0, bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(loop.ack.size()));

    /* The next gap opens after a silent ACK, so last_ack_ (0) trails next_
     * (4092): it earns exactly two ACKs, both for 4092 — the offset update the
     * sender has not seen, then the duplicate that rewinds it. */
    feed_data(rx, 2 * 4092, bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(loop.ack.size()));

    for (size_t i = 1; i <= 2; ++i) {
        next = 0;
        credits = 0;
        const std::string &a = loop.ack[i];
        TEST_ASSERT_TRUE(
            decode_ack(reinterpret_cast<const uint8_t *>(a.data()), a.size(), next, credits));
        TEST_ASSERT_EQUAL_UINT32(4092u, next);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Active),
                            static_cast<uint8_t>(rx.state()));
}

static void test_retransmit_contract(void) {
    Loop loop;
    int reads = 0;
    auto read = [&](uint32_t, uint8_t *, size_t) {
        reads++;
        return true;
    };
    auto send = [&](const std::string &p) { return loop.send_data(p); };

    /* Nothing sent yet: nothing unacknowledged. */
    XferSender fresh(20 * 4092, 4092, read, send);
    TEST_ASSERT_FALSE(fresh.retransmit());

    /* After Done there is nothing left to rewind either. */
    XferSender small(4092, 4092, read, send);
    small.pump();
    std::string ack;
    encode_ack(4092, INITIAL_CREDITS, ack);
    small.on_ack(reinterpret_cast<const uint8_t *>(ack.data()), ack.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(XferState::Done),
                            static_cast<uint8_t>(small.state()));
    TEST_ASSERT_FALSE(small.retransmit());

    /* Bytes in flight: the owner's timer rewinds to the acknowledged offset. */
    fresh.pump();
    TEST_ASSERT_TRUE(fresh.sent() > fresh.acked());
    TEST_ASSERT_TRUE(fresh.retransmit());
    TEST_ASSERT_EQUAL_UINT32(fresh.acked(), fresh.sent());
    TEST_ASSERT_EQUAL_UINT32(0u, fresh.sent());

    /* A zero-size transfer is Done at construction. */
    XferSender empty(0, 4092,
                     [&](uint32_t, uint8_t *, size_t) {
                         reads++;
                         return true;
                     },
                     send);
    TEST_ASSERT_FALSE(empty.retransmit());
}

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_check_value);
    RUN_TEST(test_crc32_rom_reference);
    RUN_TEST(test_crc32_incremental_every_split);
    RUN_TEST(test_receiver_crc_matches_payload);
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
    RUN_TEST(test_lossy_drop_mid_data);
    RUN_TEST(test_lossy_drop_earlier_mid_data);
    RUN_TEST(test_lossy_drop_last_data);
    RUN_TEST(test_lossy_drop_first_data);
    RUN_TEST(test_lossy_drop_final_ack);
    RUN_TEST(test_lossy_lossless_counts);
    RUN_TEST(test_interleaved_drop_mid_data);
    RUN_TEST(test_interleaved_drop_first_data);
    RUN_TEST(test_interleaved_lossless);
    RUN_TEST(test_sender_one_rewind_per_acked);
    RUN_TEST(test_receiver_at_most_two_acks_per_gap);
    RUN_TEST(test_retransmit_contract);
    return UNITY_END();
}
