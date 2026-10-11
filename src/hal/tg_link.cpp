/**
 * @file      tg_link.cpp
 * @brief     tg_link_request() — see tg_link.h.
 *
 * One tg.* bridge call over PagerLink (D-H7). tg.chats / tg.messages open a
 * bulk receiver whose odd id is added to the params as "xfer"; the phone then
 * answers inline or streams the body. Blocking; run on a worker task/thread.
 * The params copy carries the bridge bearer and is scrubbed before returning.
 */
#include "tg_link.h"

#include <cstdlib>
#include <string>

#include "link.h"
#include "link/link_json.h"
#include "secrets.h"

// Map a failed hw_link_call() to the UI-ready err / status the caller expects.
static void tg_link_fail(const HwLinkResult &r, int &status, std::string &err)
{
    if (r.code == "disconnected") {
        err = "Phone disconnected";
    } else if (r.code == "unauthorized") {
        status = 401;
        err = "HTTP 401";
    } else if (r.code == "http_error") {
        char *end = nullptr;
        const long v = strtol(r.msg.c_str(), &end, 10);
        if (end == r.msg.c_str() || *end != '\0' || v <= 0) {
            status = 0;
            err = "HTTP error";
        } else {
            status = static_cast<int>(v);
            err = "HTTP " + std::to_string(status);
        }
    } else if (r.code == "timeout") {
        err = "Phone timeout";
    } else {
        err = "phone: " + r.code + ": " + r.msg;
    }
}

bool tg_link_request(const char *method, const std::string &params_json, std::string &body_out,
                     int &status, std::string &err)
{
    err.clear();
    body_out.clear();
    status = 0;

    if (!hw_link_connected()) {
        err = "Phone disconnected";
        return false;
    }

    const std::string m = method ? method : "";
    const bool bulk = (m == "tg.chats" || m == "tg.messages");

    uint16_t xfer = 0;
    if (bulk) {
        xfer = hw_link_accept_open(TG_LINK_MAX_BODY);
        if (xfer == 0) {
            err = hw_link_connected() ? "phone: busy" : "Phone disconnected";
            return false;
        }
    }

    // Drop the receiver on every exit path, exactly once.
    struct XferGuard {
        uint16_t id;
        ~XferGuard() { if (id) hw_link_accept_close(id); }
    };
    XferGuard guard = {xfer};

    // Add the receiver id before the closing '}' (no comma when empty; a
    // missing '}' means the caller sent non-object text).
    std::string p = params_json;
    if (bulk) {
        const size_t close = p.rfind('}');
        if (close == std::string::npos) {
            err = "phone: bad params";
            return false;
        }
        size_t i = close;
        while (i > 0 && (p[i - 1] == ' ' || p[i - 1] == '\t' || p[i - 1] == '\r' ||
                         p[i - 1] == '\n')) {
            --i;
        }
        const bool empty = (i > 0 && p[i - 1] == '{');
        p.insert(close, std::string(empty ? "" : ",") + "\"xfer\":" + std::to_string(xfer));
    }

    const HwLinkResult r = hw_link_call(m.c_str(), p, TG_LINK_TIMEOUT_MS);
    hal::secret_scrub(p);

    if (!r.ok) {
        tg_link_fail(r, status, err);
        return false;
    }

    pagerlink::JsonObject obj;
    if (!obj.parse(r.json)) {
        err = "phone: bad reply";
        return false;
    }

    if (obj.has("body")) {
        if (!obj.get_raw("body", body_out)) {
            err = "phone: bad reply";
            return false;
        }
    } else if (bulk && obj.has("xfer")) {
        int64_t id = 0, size = 0, crc = 0;
        if (!obj.get_int("xfer", id) || !obj.get_int("size", size) || !obj.get_int("crc32", crc) ||
            id != xfer || size < 0 || size > static_cast<int64_t>(TG_LINK_MAX_BODY) || crc < 0 ||
            crc > 0xFFFFFFFFLL) {
            err = "phone: bad reply";
            return false;
        }
        if (!hw_link_accept_wait(xfer, static_cast<uint32_t>(size), static_cast<uint32_t>(crc),
                                 body_out, TG_LINK_TIMEOUT_MS)) {
            err = hw_link_connected() ? "phone: transfer failed" : "Phone disconnected";
            return false;
        }
    } else {
        body_out = "{}";
    }

    status = 200;
    return true;
}
