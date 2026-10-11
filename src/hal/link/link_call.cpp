/**
 * @file      link_call.cpp
 * @brief     hw_link_* pager-initiated requests, bulk sends and event
 *            subscriptions (the hal/link.h API).
 *
 * These wrappers bridge a caller's worker task to the link service task: the
 * service_* entry points run the work on the link task and call back with the
 * answer, while the hw_link_* functions here block the caller on a small
 * one-shot condition variable until that callback fires. The callback holds a
 * shared_ptr so a late answer after a wait gave up is still delivered safely.
 */
#include "hal/link.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "hal/link/link_service.h"

namespace {

/** One-shot completion: the link-task callback writes, the caller waits. */
struct Wait {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    HwLinkResult r;
};

/** Extra time on top of the request timeout before hw_link_call() stops waiting
 * even for a callback the service never delivered. */
constexpr uint32_t WAIT_GUARD_MS = 5000;

}  // namespace

HwLinkResult hw_link_call(const char *method, const std::string &params_json,
                          uint32_t timeout_ms) {
    HwLinkResult out;

    /* No std::make_shared: the hardware toolchain targets C++11. */
    std::shared_ptr<Wait> w(new Wait);
    const bool started = pagerlink::service_call(
        method ? method : "", params_json, timeout_ms,
        [w](bool ok, const std::string &result_json, const pagerlink::RpcError &err) {
            std::lock_guard<std::mutex> lock(w->m);
            w->ok = ok;
            w->r.ok = ok;
            w->r.json = result_json;
            w->r.code = err.code;
            w->r.msg = err.msg;
            w->done = true;
            w->cv.notify_all();
        });

    if (!started) {
        const bool conn = pagerlink::service_connected();
        out.ok = false;
        out.code = conn ? "busy" : "disconnected";
        out.msg = conn ? "link call refused" : "phone not connected";
        return out;
    }

    std::unique_lock<std::mutex> lock(w->m);
    if (!w->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms + WAIT_GUARD_MS),
                        [w] { return w->done; })) {
        out.ok = false;
        out.code = "timeout";
        out.msg = "no answer";
        return out;
    }
    return w->r;
}

bool hw_link_send_bytes(uint16_t xfer, const uint8_t *data, size_t len, uint32_t stall_ms) {
    /* No std::make_shared: the hardware toolchain targets C++11. */
    std::shared_ptr<Wait> w(new Wait);
    const bool started =
        pagerlink::service_send_bytes(xfer, data, len, stall_ms, [w](bool ok) {
            std::lock_guard<std::mutex> lock(w->m);
            w->ok = ok;
            w->done = true;
            w->cv.notify_all();
        });
    if (!started) {
        return false;
    }

    /* The service guarantees done (every byte ACKed, send failure, stall or
     * disconnect), so no guard is needed here. */
    std::unique_lock<std::mutex> lock(w->m);
    w->cv.wait(lock, [w] { return w->done; });
    return w->ok;
}

int hw_link_subscribe(const char *event, HwLinkEventFn fn) {
    if (!event || !fn) {
        return 0;
    }
    return pagerlink::service_subscribe(event, std::move(fn));
}

void hw_link_unsubscribe(int token) { pagerlink::service_unsubscribe(token); }

uint16_t hw_link_accept_open(uint32_t max_size) { return pagerlink::service_accept_open(max_size); }

bool hw_link_accept_wait(uint16_t xfer, uint32_t size, uint32_t crc32, std::string &out,
                         uint32_t stall_ms) {
    struct AcceptWait {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
        std::string bytes;
    };
    /* No std::make_shared: the hardware toolchain targets C++11. */
    std::shared_ptr<AcceptWait> w(new AcceptWait);
    const bool started = pagerlink::service_accept_expect(
        xfer, size, crc32, stall_ms, [w](bool ok, std::string &bytes) {
            std::lock_guard<std::mutex> lock(w->m);
            w->ok = ok;
            w->bytes.swap(bytes);
            w->done = true;
            w->cv.notify_all();
        });
    if (!started) {
        return false;
    }

    /* The service guarantees done (all bytes + CRC check, stall, disconnect),
     * so no guard is needed here — the same contract as hw_link_send_bytes(). */
    std::unique_lock<std::mutex> lock(w->m);
    w->cv.wait(lock, [w] { return w->done; });
    if (w->ok) {
        out.swap(w->bytes);
    }
    return w->ok;
}

void hw_link_accept_close(uint16_t xfer) { pagerlink::service_accept_close(xfer); }
