/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/link/link_rpc.{h,cpp}.
 *
 * Drives two Rpc instances (a Phone and a Pager) against each other over a
 * loopback transport: each SendFn pushes the encoded message onto the peer's
 * std::deque and pump() delivers queued messages until both are empty. A fake
 * millisecond clock lets the tests step time across the uint32 wrap. Covers the
 * message-layer contract in PROTOCOL.md §3: request/response correlation,
 * events, bulk passthrough, id parity and wrapping (including skipping ids
 * still outstanding), timeouts across the uint32 wrap, late/duplicate
 * responses, MAX_PENDING and disconnect.
 */
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>

#include "hal/link/link_frame.h"
#include "hal/link/link_rpc.h"

using namespace pagerlink;

void setUp(void) {}
void tearDown(void) {}

/* --- fake transport ------------------------------------------------------ */

static uint32_t g_now = 0;
static std::deque<std::string> g_to_phone; /* messages addressed to the phone */
static std::deque<std::string> g_to_pager; /* messages addressed to the pager */

static uint32_t fake_clock(void) { return g_now; }
static bool send_to_pager(const std::string &m) { g_to_pager.push_back(m); return true; }
static bool send_to_phone(const std::string &m) { g_to_phone.push_back(m); return true; }

static void reset_link(void) {
    g_now = 0;
    g_to_phone.clear();
    g_to_pager.clear();
}

/* Deliver every queued message (and any it generates) until both sides idle. */
static void pump(Rpc &phone, Rpc &pager) {
    for (int i = 0; i < 4096 && (!g_to_phone.empty() || !g_to_pager.empty()); ++i) {
        if (!g_to_pager.empty()) {
            std::string m = std::move(g_to_pager.front());
            g_to_pager.pop_front();
            pager.on_message(reinterpret_cast<const uint8_t *>(m.data()), m.size());
        }
        if (!g_to_phone.empty()) {
            std::string m = std::move(g_to_phone.front());
            g_to_phone.pop_front();
            phone.on_message(reinterpret_cast<const uint8_t *>(m.data()), m.size());
        }
    }
}

/* --- frame inspection ---------------------------------------------------- */

static uint16_t frame_id(const std::string &m) {
    return static_cast<uint16_t>(static_cast<uint8_t>(m[2]) |
                                 (static_cast<uint8_t>(m[3]) << 8));
}
static uint8_t frame_type(const std::string &m) { return static_cast<uint8_t>(m[1]); }
static std::string frame_payload(const std::string &m) { return m.substr(HEADER_SIZE); }

static void feed(Rpc &r, const std::string &msg) {
    r.on_message(reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
}

static void send_res_id(Rpc &r, uint16_t id, const std::string &payload) {
    std::string m;
    TEST_ASSERT_TRUE(encode_message(MsgType::Res, id,
                                    reinterpret_cast<const uint8_t *>(payload.data()),
                                    payload.size(), m));
    feed(r, m);
}

/* --- tests --------------------------------------------------------------- */

static void test_phone_to_pager_ping_round_trip(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    pager.handle("sys.ping", [](const JsonObject &params, JsonWriter &result, RpcError &) {
        int64_t t = 0;
        params.get_int("t", t);
        result.begin_object().key("t").int_val(t).end_object();
        return true;
    });

    bool ok = false;
    bool called = false;
    std::string res;
    RpcError err;
    TEST_ASSERT_TRUE(phone.call("sys.ping", "{\"t\":42}",
                                [&](bool o, const std::string &r, const RpcError &e) {
                                    called = true;
                                    ok = o;
                                    res = r;
                                    err = e;
                                }));
    pump(phone, pager);

    TEST_ASSERT_TRUE(called);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("{\"t\":42}", res.c_str());
    TEST_ASSERT_EQUAL_STRING("", err.code.c_str());
}

static void test_pager_to_phone_call(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    phone.handle("sys.ping", [](const JsonObject &, JsonWriter &result, RpcError &) {
        result.begin_object().key("t").int_val(7).end_object();
        return true;
    });

    bool ok = false;
    TEST_ASSERT_TRUE(pager.call("sys.ping", "",
                                [&](bool o, const std::string &, const RpcError &) { ok = o; }));

    /* The REQ is queued but not yet delivered: inspect the raw bytes. */
    TEST_ASSERT_FALSE(g_to_phone.empty());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::Req),
                            frame_type(g_to_phone.front()));
    TEST_ASSERT_EQUAL_UINT16(1, frame_id(g_to_phone.front())); /* pager's first id is odd */

    pump(phone, pager);
    TEST_ASSERT_TRUE(ok);
}

static void test_id_parity(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    auto noop = [](bool, const std::string &, const RpcError &) {};

    uint16_t pid[3];
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(phone.call("m", "", noop));
        pid[i] = frame_id(g_to_pager.back());
    }
    TEST_ASSERT_EQUAL_UINT16(2, pid[0]);
    TEST_ASSERT_EQUAL_UINT16(4, pid[1]);
    TEST_ASSERT_EQUAL_UINT16(6, pid[2]);

    uint16_t gid[2];
    for (int i = 0; i < 2; ++i) {
        TEST_ASSERT_TRUE(pager.call("m", "", noop));
        gid[i] = frame_id(g_to_phone.back());
    }
    TEST_ASSERT_EQUAL_UINT16(1, gid[0]);
    TEST_ASSERT_EQUAL_UINT16(3, gid[1]);
}

static void test_ids_skip_pending(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    uint32_t cb_calls = 0;
    auto cb = [&](bool, const std::string &, const RpcError &) { cb_calls++; };
    const uint32_t LONG = 0x7FFFFFFFu; /* stays well in the future */

    /* Occupy id 2 and keep it outstanding for the whole walk. */
    TEST_ASSERT_TRUE(phone.call("m", "", cb, LONG));
    TEST_ASSERT_EQUAL_UINT16(2, frame_id(g_to_pager.back()));
    g_to_pager.clear(); /* drop the REQ; its callback never runs */

    /* Walk the even ids 4..65534, freeing each with a fabricated RES. */
    for (uint32_t i = 0; i < 32766; ++i) {
        TEST_ASSERT_TRUE(phone.call("m", "", cb, LONG));
        std::string raw = std::move(g_to_pager.front());
        g_to_pager.pop_front();
        const uint16_t id = frame_id(raw);
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(4 + 2 * i), id);
        send_res_id(phone, id, "{}");
    }
    TEST_ASSERT_EQUAL_UINT32(32766u, cb_calls);

    /* next_id_ has wrapped to 2, which is still pending -> skip to 4. */
    TEST_ASSERT_TRUE(phone.call("m", "", cb, LONG));
    TEST_ASSERT_EQUAL_UINT16(4, frame_id(g_to_pager.back()));
    TEST_ASSERT_EQUAL_UINT32(32766u, cb_calls);

    phone.disconnect();
}

static void test_wrap_after_65534_is_2(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    auto noop = [](bool, const std::string &, const RpcError &) {};
    uint16_t last = 0;
    for (uint32_t i = 0; i < 32767; ++i) {
        TEST_ASSERT_TRUE(phone.call("m", "", noop));
        last = frame_id(g_to_pager.back());
        g_to_pager.clear(); /* send() returned true without delivery */
        phone.disconnect(); /* stay under MAX_PENDING */
    }
    TEST_ASSERT_EQUAL_UINT16(65534, last);

    TEST_ASSERT_TRUE(phone.call("m", "", noop));
    TEST_ASSERT_EQUAL_UINT16(2, frame_id(g_to_pager.back()));
}

static void test_unknown_method(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    bool ok = true;
    RpcError err;
    TEST_ASSERT_TRUE(phone.call("no.such.method", "",
                                [&](bool o, const std::string &, const RpcError &e) {
                                    ok = o;
                                    err = e;
                                }));
    pump(phone, pager);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("unsupported", err.code.c_str());
    TEST_ASSERT_EQUAL_UINT32(1u, pager.stats().unsupported);
}

static void test_handler_error_code(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    pager.handle("fs.write", [](const JsonObject &, JsonWriter &, RpcError &err) {
        err.code = "storage_full";
        err.msg = "disk full";
        return false;
    });

    bool ok = true;
    RpcError err;
    TEST_ASSERT_TRUE(phone.call("fs.write", "{}",
                                [&](bool o, const std::string &, const RpcError &e) {
                                    ok = o;
                                    err = e;
                                }));
    pump(phone, pager);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("storage_full", err.code.c_str());
    TEST_ASSERT_EQUAL_STRING("disk full", err.msg.c_str());
}

static void test_handler_empty_code_is_internal(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    pager.handle("fs.fail", [](const JsonObject &, JsonWriter &, RpcError &) { return false; });

    bool ok = true;
    RpcError err;
    TEST_ASSERT_TRUE(phone.call("fs.fail", "",
                                [&](bool o, const std::string &, const RpcError &e) {
                                    ok = o;
                                    err = e;
                                }));
    pump(phone, pager);

    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("internal", err.code.c_str());
}

static void test_timeout_then_late_response(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    int calls = 0;
    bool ok = true;
    RpcError err;
    TEST_ASSERT_TRUE(phone.call("m", "",
                                [&](bool o, const std::string &, const RpcError &e) {
                                    calls++;
                                    ok = o;
                                    err = e;
                                },
                                100));
    const uint16_t id = frame_id(g_to_pager.back());

    g_now = 101;
    phone.tick();

    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_STRING("timeout", err.code.c_str());
    TEST_ASSERT_EQUAL_UINT32(1u, phone.stats().timeouts);

    /* The response arrives after the timeout: dropped as late, cb not re-run. */
    send_res_id(phone, id, "{}");
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_UINT32(1u, phone.stats().late_responses);
}

static void test_timeout_across_uint32_wrap(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    int calls = 0;
    RpcError err;
    g_now = 0xFFFFFF00u;
    TEST_ASSERT_TRUE(phone.call("m", "",
                                [&](bool, const std::string &, const RpcError &e) {
                                    calls++;
                                    err = e;
                                },
                                0x200));

    g_now = 0xFFFFFF00u + 0x1FFu; /* just short of the deadline */
    phone.tick();
    TEST_ASSERT_EQUAL_INT(0, calls);

    g_now = 0xFFFFFF00u + 0x201u; /* two past the just-short point */
    phone.tick();
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_STRING("timeout", err.code.c_str());
}

static void test_duplicate_response_is_late(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    pager.handle("m", [](const JsonObject &, JsonWriter &result, RpcError &) {
        result.begin_object().end_object();
        return true;
    });

    int calls = 0;
    bool ok = false;
    TEST_ASSERT_TRUE(phone.call("m", "", [&](bool o, const std::string &, const RpcError &) {
        calls++;
        ok = o;
    }));
    const uint16_t id = frame_id(g_to_pager.back());

    pump(phone, pager);
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_TRUE(ok);

    send_res_id(phone, id, "{}"); /* duplicate */
    TEST_ASSERT_EQUAL_INT(1, calls);
    TEST_ASSERT_EQUAL_UINT32(1u, phone.stats().late_responses);
}

static void test_max_pending(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    auto noop = [](bool, const std::string &, const RpcError &) {};
    for (size_t i = 0; i < Rpc::MAX_PENDING; ++i) {
        TEST_ASSERT_TRUE(phone.call("m", "", noop, 0x7FFFFFFFu));
    }
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(Rpc::MAX_PENDING),
                             static_cast<uint32_t>(phone.pending()));

    TEST_ASSERT_FALSE(phone.call("m", "", noop, 0x7FFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(Rpc::MAX_PENDING),
                             static_cast<uint32_t>(phone.pending()));
}

static void test_disconnect_fails_all(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    int calls = 0;
    int disconnected = 0;
    RpcError last;
    auto cb = [&](bool ok, const std::string &, const RpcError &e) {
        calls++;
        if (!ok) disconnected++;
        last = e;
    };
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(phone.call("m", "", cb, 0x7FFFFFFFu));
    }

    phone.disconnect();
    TEST_ASSERT_EQUAL_INT(3, calls);
    TEST_ASSERT_EQUAL_INT(3, disconnected);
    TEST_ASSERT_EQUAL_STRING("disconnected", last.code.c_str());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(phone.pending()));
}

static void test_wrong_parity_request(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    /* Phone must receive odd REQ ids; craft an even one. */
    const std::string payload = "{\"m\":\"x\"}";
    std::string req;
    TEST_ASSERT_TRUE(encode_message(MsgType::Req, 2,
                                    reinterpret_cast<const uint8_t *>(payload.data()),
                                    payload.size(), req));
    feed(phone, req);

    TEST_ASSERT_FALSE(g_to_pager.empty());
    const std::string reply = g_to_pager.front();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::Err), frame_type(reply));
    TEST_ASSERT_EQUAL_UINT16(2, frame_id(reply));

    JsonObject obj;
    std::string code;
    TEST_ASSERT_TRUE(obj.parse(frame_payload(reply)));
    TEST_ASSERT_TRUE(obj.get_string("code", code));
    TEST_ASSERT_EQUAL_STRING("invalid_argument", code.c_str());
}

static void test_request_payload_not_json(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    const std::string payload = "not json";
    std::string req;
    TEST_ASSERT_TRUE(encode_message(MsgType::Req, 1, /* odd: valid for the phone */
                                    reinterpret_cast<const uint8_t *>(payload.data()),
                                    payload.size(), req));
    feed(phone, req);

    TEST_ASSERT_FALSE(g_to_pager.empty());
    const std::string reply = g_to_pager.front();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::Err), frame_type(reply));
    TEST_ASSERT_EQUAL_UINT16(1, frame_id(reply));

    JsonObject obj;
    std::string code;
    TEST_ASSERT_TRUE(obj.parse(frame_payload(reply)));
    TEST_ASSERT_TRUE(obj.get_string("code", code));
    TEST_ASSERT_EQUAL_STRING("bad_request", code.c_str());
    TEST_ASSERT_EQUAL_UINT32(1u, phone.stats().bad_requests);
}

static void test_malformed_message(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    const uint8_t short_msg[3] = {PROTO_VERSION, 0, 0};
    phone.on_message(short_msg, sizeof(short_msg));
    TEST_ASSERT_EQUAL_UINT32(1u, phone.stats().malformed);

    const uint8_t bad_ver[7] = {9, 0, 0, 0, 0, 0, 0};
    phone.on_message(bad_ver, sizeof(bad_ver));
    TEST_ASSERT_EQUAL_UINT32(2u, phone.stats().malformed);
}

static void test_event_delivery(void) {
    reset_link();
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    std::string seen_event;
    int64_t seen_pct = -1;
    pager.on_event([&](const std::string &e, const JsonObject &p) {
        seen_event = e;
        p.get_int("pct", seen_pct);
    });

    const std::string payload = "{\"e\":\"evt.battery\",\"p\":{\"pct\":50}}";
    std::string evt;
    TEST_ASSERT_TRUE(encode_message(MsgType::Evt, 0,
                                    reinterpret_cast<const uint8_t *>(payload.data()),
                                    payload.size(), evt));
    feed(pager, evt);

    TEST_ASSERT_EQUAL_STRING("evt.battery", seen_event.c_str());
    TEST_ASSERT_EQUAL_INT64(50, seen_pct);
}

static void test_link_reset_event(void) {
    reset_link();
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    std::string seen_event;
    std::string seen_reason;
    pager.on_event([&](const std::string &e, const JsonObject &p) {
        seen_event = e;
        p.get_string("reason", seen_reason);
    });

    pager.on_link_reset(ResetReason::SeqGap);

    TEST_ASSERT_EQUAL_STRING("link.reset", seen_event.c_str());
    TEST_ASSERT_EQUAL_STRING("seq_gap", seen_reason.c_str());
    TEST_ASSERT_EQUAL_UINT32(1u, pager.stats().link_resets);
}

static void test_bulk_passthrough(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    int bulk_calls = 0;
    MsgType seen_type = MsgType::Req;
    uint16_t seen_xfer = 0;
    std::string seen_payload;
    pager.on_bulk([&](MsgType t, uint16_t xfer, const uint8_t *p, size_t n) {
        bulk_calls++;
        seen_type = t;
        seen_xfer = xfer;
        seen_payload.assign(reinterpret_cast<const char *>(p), n);
    });

    std::string data;
    TEST_ASSERT_TRUE(encode_data(0, reinterpret_cast<const uint8_t *>("abc"), 3, data));
    TEST_ASSERT_TRUE(phone.send_bulk(MsgType::Data, 7, data));
    pump(phone, pager);

    TEST_ASSERT_EQUAL_INT(1, bulk_calls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::Data), static_cast<uint8_t>(seen_type));
    TEST_ASSERT_EQUAL_UINT16(7, seen_xfer);
    TEST_ASSERT_EQUAL_UINT32(7u, static_cast<uint32_t>(seen_payload.size())); /* 4 + 3 */

    uint32_t off = 1;
    const uint8_t *bytes = nullptr;
    size_t n = 0;
    TEST_ASSERT_TRUE(decode_data(reinterpret_cast<const uint8_t *>(seen_payload.data()),
                                 seen_payload.size(), off, bytes, n));
    TEST_ASSERT_EQUAL_UINT32(0u, off);
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(n));
}

static void test_event_emitted(void) {
    reset_link();
    Rpc pager(Role::Pager, send_to_phone, fake_clock);
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    TEST_ASSERT_TRUE(pager.emit("evt.battery", "{\"pct\":9}"));
    TEST_ASSERT_FALSE(g_to_phone.empty());

    const std::string evt = g_to_phone.front();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::Evt), frame_type(evt));
    TEST_ASSERT_EQUAL_UINT16(0, frame_id(evt));

    std::string seen_event;
    int64_t pct = 0;
    phone.on_event([&](const std::string &e, const JsonObject &p) {
        seen_event = e;
        p.get_int("pct", pct);
    });
    pump(phone, pager);
    TEST_ASSERT_EQUAL_STRING("evt.battery", seen_event.c_str());
    TEST_ASSERT_EQUAL_INT64(9, pct);
}

static void test_reentrant_timeout_callback(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    int first_calls = 0;
    bool second_called = false;
    TEST_ASSERT_TRUE(phone.call("m", "",
                                [&](bool, const std::string &, const RpcError &) {
                                    first_calls++;
                                    /* Issuing a new request from inside a
                                     * timeout callback must be safe. */
                                    phone.call("m2", "",
                                               [&](bool, const std::string &, const RpcError &) {
                                                   second_called = true;
                                               },
                                               0x7FFFFFFFu);
                                },
                                10));

    g_now = 11;
    phone.tick(); /* must not crash */

    TEST_ASSERT_EQUAL_INT(1, first_calls);
    TEST_ASSERT_FALSE(second_called);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(phone.pending()));
    phone.disconnect();
}

static void test_defer_runs_after_reply(void) {
    reset_link();
    Rpc pager(Role::Pager, send_to_phone, fake_clock);

    int defer_calls = 0;
    int sent_when_deferred = -1;
    pager.handle("fs.read", [&](const JsonObject &, JsonWriter &result, RpcError &) {
        result.begin_object().end_object();
        pager.defer([&]() {
            defer_calls++;
            sent_when_deferred = static_cast<int>(g_to_phone.size());
        });
        return true;
    });

    const std::string payload = "{\"m\":\"fs.read\"}";
    std::string req;
    TEST_ASSERT_TRUE(encode_message(MsgType::Req, 2, /* even: valid for the pager */
                                    reinterpret_cast<const uint8_t *>(payload.data()),
                                    payload.size(), req));
    feed(pager, req);

    TEST_ASSERT_EQUAL_INT(1, defer_calls);
    /* The deferred action must have observed the RES already queued. */
    TEST_ASSERT_EQUAL_INT(1, sent_when_deferred);
    TEST_ASSERT_FALSE(g_to_phone.empty());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::Res), frame_type(g_to_phone.front()));
}

static void test_defer_outside_handler_runs_now(void) {
    reset_link();
    Rpc phone(Role::Phone, send_to_pager, fake_clock);

    int calls = 0;
    phone.defer([&]() { calls++; });
    TEST_ASSERT_EQUAL_INT(1, calls);

    phone.defer(nullptr); /* an empty action is a no-op */
    TEST_ASSERT_EQUAL_INT(1, calls);
}

static void test_hal_error_code(void) {
    TEST_ASSERT_EQUAL_STRING("invalid_argument", hal_error_code(HalError::InvalidArgument));
    TEST_ASSERT_EQUAL_STRING("unsupported", hal_error_code(HalError::NotSupported));
    TEST_ASSERT_EQUAL_STRING("internal", hal_error_code(HalError::InternalError));
    TEST_ASSERT_EQUAL_STRING("storage_full", hal_error_code(HalError::StorageFull));
    TEST_ASSERT_EQUAL_STRING("wifi_offline", hal_error_code(HalError::WifiOffline));
    TEST_ASSERT_EQUAL_STRING("http_error", hal_error_code(HalError::HttpError));
    TEST_ASSERT_EQUAL_STRING("unauthorized", hal_error_code(HalError::Unauthorized));
    TEST_ASSERT_EQUAL_STRING("disconnected", hal_error_code(HalError::LinkOffline));
    TEST_ASSERT_EQUAL_STRING("internal", hal_error_code(HalError::Ok));
}

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_phone_to_pager_ping_round_trip);
    RUN_TEST(test_pager_to_phone_call);
    RUN_TEST(test_id_parity);
    RUN_TEST(test_ids_skip_pending);
    RUN_TEST(test_wrap_after_65534_is_2);
    RUN_TEST(test_unknown_method);
    RUN_TEST(test_handler_error_code);
    RUN_TEST(test_handler_empty_code_is_internal);
    RUN_TEST(test_timeout_then_late_response);
    RUN_TEST(test_timeout_across_uint32_wrap);
    RUN_TEST(test_duplicate_response_is_late);
    RUN_TEST(test_max_pending);
    RUN_TEST(test_disconnect_fails_all);
    RUN_TEST(test_wrong_parity_request);
    RUN_TEST(test_request_payload_not_json);
    RUN_TEST(test_malformed_message);
    RUN_TEST(test_event_delivery);
    RUN_TEST(test_link_reset_event);
    RUN_TEST(test_bulk_passthrough);
    RUN_TEST(test_event_emitted);
    RUN_TEST(test_reentrant_timeout_callback);
    RUN_TEST(test_defer_runs_after_reply);
    RUN_TEST(test_defer_outside_handler_runs_now);
    RUN_TEST(test_hal_error_code);
    return UNITY_END();
}
