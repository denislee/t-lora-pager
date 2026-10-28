/**
 * @file      handlers_settings.cpp
 * @brief     PagerLink settings.* handlers (PROTOCOL.md §6.4, IOS_SETTINGS_PLAN.md W6).
 *
 * Thin JSON <-> registry glue: the device registry (hal/settings.h) owns
 * validation, the all-or-nothing batch, the revision and every store. Access
 * control is the same as fs.*: the bonded, encrypted BLE link (or the
 * emulator's loopback TCP).
 *
 * settings.set queues its hardware applies instead of posting them, and posts
 * them from rpc.defer, so conn.bt=false cannot drop the link before the RES
 * has gone out (T3).
 *
 * The registry observer feeds service_note_settings_changed(), which
 * coalesces the ids and emits one evt.settings.changed per 250 ms from
 * service_tick().
 */
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../core/ui_post.h"
#include "../settings.h"

#include "hal/link/link_json.h"
#include "hal/link/link_log.h"
#include "hal/link/link_rpc.h"
#include "hal/link/link_service.h"

namespace pagerlink {
namespace {

void to_rpc(const settings::Error &e, RpcError &err) {
    err.code = e.code.empty() ? "internal" : e.code;
    err.msg = e.msg;
}

bool handle_schema(const JsonObject &params, JsonWriter &result, RpcError &err) {
    settings::Error e;
    if (settings::registry().schema_json(params, result, e)) return true;
    to_rpc(e, err);
    return false;
}

bool handle_get(const JsonObject &params, JsonWriter &result, RpcError &err) {
    settings::Error e;
    if (settings::registry().get_json(params, result, e)) return true;
    to_rpc(e, err);
    return false;
}

/** "a,b,c" of the ids in values{} for the log line (never the values). */
std::string id_list(const JsonObject &params) {
    JsonObject values;
    std::string out;
    if (!params.get_object("values", values)) return out;
    for (const std::string &k : values.keys()) {
        if (!out.empty()) out += ",";
        out += k;
    }
    return out;
}

}  // namespace

void register_settings_handlers(Rpc &rpc) {
    /* One observer for the process: every change, from either UI, feeds the
     * coalesced evt.settings.changed of whichever phone is connected. */
    static bool observing = false;
    if (!observing) {
        observing = true;
        settings::registry().subscribe([](uint32_t rev, const std::vector<std::string> &ids) {
            service_note_settings_changed(rev, ids);
        });
    }

    rpc.handle("settings.schema", handle_schema);
    rpc.handle("settings.get", handle_get);

    rpc.handle("settings.set", [&rpc](const JsonObject &params, JsonWriter &result,
                                      RpcError &err) -> bool {
        LINK_LOG("settings.set ids=[%s]", id_list(params).c_str());
        std::shared_ptr<std::vector<std::function<void()>>> applies(
            new std::vector<std::function<void()>>());
        settings::set_apply_sink(applies.get());
        settings::Error e;
        const bool ok = settings::registry().set_json(params, result, e);
        settings::set_apply_sink(nullptr);
        /* Applies of ids written before a failing setter still run. */
        if (!applies->empty()) {
            rpc.defer([applies] {
                for (const std::function<void()> &fn : *applies) core::ui_post(fn);
            });
        }
        if (!ok) to_rpc(e, err);
        return ok;
    });

    rpc.handle("settings.action", [](const JsonObject &params, JsonWriter &result,
                                     RpcError &err) -> bool {
        std::string id;
        if (!params.get_string("id", id)) {
            err.code = "invalid_argument";
            err.msg = "id required";
            return false;
        }
        const settings::ActionDef *a = settings::registry().find_action(id);
        if (!a) {
            err.code = "invalid_argument";
            err.msg = id + ": unknown action";
            return false;
        }
        if (!a->supported || !a->run) {
            err.code = "unsupported";
            err.msg = id + ": not on this board";
            return false;
        }
        JsonObject p;
        if (params.has("p") && !params.get_object("p", p)) {
            err.code = "invalid_argument";
            err.msg = "p must be an object";
            return false;
        }
        LINK_LOG("settings.action %s", id.c_str());
        settings::Error e;
        if (a->run(p, result, e)) {
            if (result.empty()) result.begin_object().end_object();
            return true;
        }
        to_rpc(e, err);
        return false;
    });
}

}  // namespace pagerlink
