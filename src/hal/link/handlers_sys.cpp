/**
 * @file      handlers_sys.cpp
 * @brief     PagerLink v1 sys.* RPC handlers (PROTOCOL.md §6).
 *
 * Reads live device state from the HAL and answers the sys family: hello,
 * info, ping and time.set. The transport/RPC plumbing lives in
 * link_service.cpp; this file only maps JSON <-> HAL calls.
 */
#include "../power.h"
#include "../storage.h"
#include "../system.h"
#include "../timezone.h"
#include "../wireless.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

#include "hal/link/link_service.h"

#ifdef ARDUINO
#include <esp32-hal-log.h>
#define LINK_LOG(...) log_i(__VA_ARGS__)
#else
#include <cstdio>
#define LINK_LOG(fmt, ...) printf("[link] " fmt "\n", ##__VA_ARGS__)
#endif


namespace pagerlink {
namespace {

/** TZ strings are IANA names (and POSIX forms): [A-Za-z0-9_/+-], 1..64 chars. */
bool tz_charset_ok(const std::string &tz) {
    if (tz.empty() || tz.size() > 64) {
        return false;
    }
    for (char c : tz) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '+' ||
                        c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

void register_sys_handlers(Rpc &rpc) {
    // sys.hello — proto check + identity/capabilities.
    rpc.handle("sys.hello", [](const JsonObject &params, JsonWriter &result,
                               RpcError &err) -> bool {
        int64_t proto = 0;
        if (!params.get_int("proto", proto)) {
            err.code = hal_error_code(HalError::InvalidArgument);
            err.msg = "proto required";
            return false;
        }
        if (proto != PROTO_VERSION) {
            err.code = "unsupported";
            err.msg = "protocol major mismatch";
            return false;
        }
        result.begin_object()
            .key("proto")
            .int_val(PROTO_VERSION)
            .key("fw")
            .str_val(hw_get_firmware_hash_string())
            .key("board")
            .str_val(board_tag())
            .key("name")
            .str_val(hw_get_ble_kb_name())
            .key("caps")
            .begin_array()
            .str_val("sys")
            .str_val("fs")
            .str_val("weather")
            .end_array()
            .key("mtu")
            .uint_val(static_cast<uint64_t>(service_packet_size() + 3))
            .end_object();
        return true;
    });

    // sys.info — a snapshot of battery, link/network state and storage.
    rpc.handle("sys.info", [](const JsonObject &, JsonWriter &result,
                              RpcError &) -> bool {
        monitor_params_t mp;
        hw_get_monitor_params(mp);

        const bool wifi_connected = hw_get_wifi_connected();
        std::string ssid;
        if (wifi_connected) {
            hw_get_wifi_ssid(ssid);
        }
        const int rssi = wifi_connected ? static_cast<int>(hw_get_wifi_rssi()) : 0;

        uint64_t int_total = 0, int_used = 0, int_free = 0;
        uint64_t sd_total = 0, sd_used = 0, sd_free = 0;
        hw_get_local_storage_info(int_total, int_used, int_free);
        hw_get_storage_info(sd_total, sd_used, sd_free);

        const uint64_t uptime_s = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        result.begin_object()
            .key("batt_pct")
            .int_val(mp.battery_percent)
            .key("charging")
            .bool_val(mp.is_charging)
            .key("uptime_s")
            .uint_val(uptime_s)
            .key("wifi")
            .begin_object()
            .key("on")
            .bool_val(hw_get_wifi_enable())
            .key("connected")
            .bool_val(wifi_connected)
            .key("ssid")
            .str_val(ssid)
            .key("rssi")
            .int_val(rssi)
            .end_object()
            .key("storage")
            .begin_object()
            .key("int")
            .begin_object()
            .key("t")
            .uint_val(int_total)
            .key("u")
            .uint_val(int_used)
            .end_object()
            .key("sd")
            .begin_object()
            .key("t")
            .uint_val(sd_total)
            .key("u")
            .uint_val(sd_used)
            .key("mounted")
            .bool_val(sd_total > 0)
            .end_object()
            .end_object()
            .end_object();
        return true;
    });

    // sys.ping — echo the caller's token.
    rpc.handle("sys.ping", [](const JsonObject &params, JsonWriter &result,
                              RpcError &err) -> bool {
        int64_t t = 0;
        if (!params.get_int("t", t)) {
            err.code = hal_error_code(HalError::InvalidArgument);
            err.msg = "t required";
            return false;
        }
        result.begin_object().key("t").int_val(t).end_object();
        return true;
    });

    // sys.time.set — set the wall clock from a unix epoch + IANA zone.
    rpc.handle("sys.time.set", [](const JsonObject &params, JsonWriter &result,
                                  RpcError &err) -> bool {
        int64_t epoch = 0;
        if (!params.get_int("epoch", epoch) || epoch < 1577836800 /* 2020-01-01 */) {
            err.code = hal_error_code(HalError::InvalidArgument);
            err.msg = "epoch out of range";
            return false;
        }
        std::string tz;
        if (!params.get_string("tz", tz) || !tz_charset_ok(tz)) {
            err.code = hal_error_code(HalError::InvalidArgument);
            err.msg = "bad tz";
            return false;
        }

        timezone_set_user_tz(tz.c_str());

        time_t t = static_cast<time_t>(epoch);
        struct tm local;
        localtime_r(&t, &local);
        hw_set_date_time(local);

        LINK_LOG("time set: epoch=%lld tz=%s", static_cast<long long>(epoch), tz.c_str());
        result.begin_object().end_object();
        return true;
    });
}

}  // namespace pagerlink
