/**
 * @file      handlers_notify.cpp
 * @brief     PagerLink messaging handlers (PROTOCOL.md §6.5): notify.post,
 *            clip.push, text.send and pager.find.
 *
 * The rules live in hal/companion_msg.cpp (vector-tested); the inbox in
 * hal/companion.cpp; everything that touches the screen goes through
 * ui_companion.h, whose functions only flag or post to the LVGL thread — a
 * handler runs on the link task with the service lock held and must not block
 * on the UI.
 */
#include <ctime>
#include <string>

#include "../companion.h"
#include "../companion_msg.h"
#include "../result.h"
#include "../../core/system_hooks.h"
#include "../../ui_companion.h"

#include "hal/link/link_json.h"
#include "hal/link/link_rpc.h"
#include "hal/link/link_service.h"

namespace pagerlink {
namespace {

bool bad(RpcError &err, const std::string &why) {
    err.code = hal_error_code(HalError::InvalidArgument);
    err.msg = why;
    return false;
}

/** Pager wall clock in unix seconds, or 0 before the clock was ever set. */
uint32_t now_epoch() {
    const time_t t = time(nullptr);
    return t > 1577836800 /* 2020-01-01 */ ? static_cast<uint32_t>(t) : 0;
}

bool handle_notify_post(const JsonObject &params, JsonWriter &result, RpcError &err) {
    hal::msg::NotifyPost np;
    std::string why;
    if (!hal::msg::parse_notify_post(params, np, why)) return bad(err, why);

    hal::msg::InboxEntry e;
    e.id = np.id;
    e.kind = "msg";
    e.title = np.title;
    e.body = np.body;
    e.src = np.src;
    e.ts = np.ts;
    e.rx = now_epoch();
    e.prio = np.prio;
    if (!hal::companion::inbox_add(e)) {
        result.begin_object().key("stored").bool_val(false).key("dup").bool_val(true).end_object();
        return true;
    }
    ui_companion_deliver(e, hal::msg::delivery_for(np.prio, ui_is_fake_sleep(),
                                                   ui_companion_can_wake()));
    result.begin_object().key("stored").bool_val(true).end_object();
    return true;
}

bool handle_clip_push(const JsonObject &params, JsonWriter &result, RpcError &err) {
    std::string text, why;
    if (!hal::msg::parse_clip_push(params, text, why)) return bad(err, why);

    if (ui_companion_editor_open()) {
        ui_companion_type(hal::msg::Target::Editor, text);
        result.begin_object().key("pasted").bool_val(true).end_object();
        return true;
    }
    hal::msg::InboxEntry e;
    e.kind = "clip";
    e.title = "Clipboard from phone";
    e.body = text;
    e.rx = now_epoch();
    hal::companion::inbox_add(e);
    ui_companion_deliver(e, hal::msg::delivery_for(hal::msg::Prio::Normal, ui_is_fake_sleep(),
                                                   ui_companion_can_wake()));
    result.begin_object().key("pasted").bool_val(false).end_object();
    return true;
}

bool handle_text_send(const JsonObject &params, JsonWriter &result, RpcError &err) {
    std::string text, why;
    hal::msg::Target target = hal::msg::Target::Kb;
    if (!hal::msg::parse_text_send(params, text, target, why)) return bad(err, why);
    if (target == hal::msg::Target::Editor && !ui_companion_editor_open()) {
        return bad(err, "editor not open");
    }
    if (target == hal::msg::Target::Kb && !ui_companion_text_input_focused()) {
        return bad(err, "no text input");
    }
    ui_companion_type(target, text);
    result.begin_object().key("typed").uint_val(text.size()).end_object();
    return true;
}

bool handle_pager_find(const JsonObject &params, JsonWriter &result, RpcError &err) {
    bool stop = false;
    if (params.has("stop") && !params.get_bool("stop", stop)) return bad(err, "stop: expected boolean");
    ui_companion_find_pager(!stop);
    result.begin_object().end_object();
    return true;
}

}  // namespace

void register_notify_handlers(Rpc &rpc) {
    rpc.handle("notify.post", handle_notify_post);
    rpc.handle("clip.push", handle_clip_push);
    rpc.handle("text.send", handle_text_send);
    rpc.handle("pager.find", handle_pager_find);
}

}  // namespace pagerlink
