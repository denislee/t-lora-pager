/**
 * @file      handlers_screen.cpp
 * @brief     PagerLink screen.capture (PROTOCOL.md §6.6).
 *
 * Answers {"job"} at once; the snapshot runs on the LVGL thread and the bytes
 * follow through screen.data (ui_screen_capture.cpp), because the RES has to
 * go out before the capture's size and CRC are known.
 */
#include <atomic>
#include <cstdio>
#include <string>

#include "../result.h"
#include "../../ui_companion.h"

#include "hal/link/link_json.h"
#include "hal/link/link_rpc.h"
#include "hal/link/link_service.h"

namespace pagerlink {
namespace {

std::atomic<uint32_t> s_next_job{1};

bool handle_screen_capture(const JsonObject &, JsonWriter &result, RpcError &err) {
    char job[16];
    std::snprintf(job, sizeof(job), "s%u", static_cast<unsigned>(s_next_job.fetch_add(1)));
    if (!ui_screen_capture_start(job)) {
        err.code = "busy";
        err.msg = "a capture is still running";
        return false;
    }
    result.begin_object().key("job").str_val(job).end_object();
    return true;
}

}  // namespace

void register_screen_handlers(Rpc &rpc) { rpc.handle("screen.capture", handle_screen_capture); }

}  // namespace pagerlink
