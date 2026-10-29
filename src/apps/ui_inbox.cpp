/**
 * @file      ui_inbox.cpp
 * @brief     Inbox app: what the phone sent (notify.post, clip.push with no
 *            editor open; PROTOCOL.md §6.5), newest first, plus "Find phone".
 *
 * State reset on onStop (ui_inbox_exit):
 *   widgets: menu, quit_btn              — ui_app_widgets_del (nulled)
 *            list, find_btn              — children of menu, nulled
 *   timers:  refresh_timer               — lv_timer_del + null
 *   state:   s_gen (last rendered inbox generation) — reset to 0
 * No tasks of its own: "Find phone" runs on ui_companion's shared worker,
 * whose result is a banner, so it may finish after this app has closed.
 */
#include "../ui_define.h"
#include "../ui_companion.h"
#include "../hal/companion.h"
#include "../hal/link.h"

#include <ctime>
#include <string>
#include <vector>

using std::string;

static lv_obj_t *menu = NULL;
static lv_obj_t *quit_btn = NULL;
static lv_obj_t *list = NULL;
static lv_obj_t *find_btn = NULL;
static lv_timer_t *refresh_timer = NULL;
static uint32_t s_gen = 0;

static void do_exit() { menu_show_deferred(); }
static void back_cb(lv_event_t *) { do_exit(); }

static string when_text(uint32_t rx)
{
    if (!rx) return "";
    time_t t = (time_t)rx;
    struct tm tm;
    localtime_r(&t, &tm);
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    char buf[24];
    if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday) {
        strftime(buf, sizeof(buf), "%H:%M", &tm);
    } else {
        strftime(buf, sizeof(buf), "%b %d %H:%M", &tm);
    }
    return buf;
}

static void rebuild();

// Backspace / Delete removes the focused message; ESC leaves the app.
static void row_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ESC) {
        do_exit();
    } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
        hal::companion::inbox_remove(idx);
        rebuild();
    }
}

static void find_cb(lv_event_t *)
{
    hw_feedback();
    ui_companion_find_phone();
}

static void clear_cb(lv_event_t *)
{
    hal::companion::inbox_clear();
    rebuild();
}

static void action_key_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_KEY && lv_event_get_key(e) == LV_KEY_ESC) do_exit();
}

static lv_obj_t *make_action(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(b, action_key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(lv_group_get_default(), b);
    return b;
}

static void rebuild()
{
    if (!list) return;
    hal::companion::inbox_mark_all_read();
    s_gen = hal::companion::inbox_generation();
    lv_obj_clean(list);

    const std::vector<hal::msg::InboxEntry> ring = hal::companion::inbox_snapshot();
    if (ring.empty()) {
        lv_obj_t *l = lv_label_create(list);
        lv_label_set_text(l, hw_link_connected()
                                 ? "No messages. Send one from the phone app."
                                 : "No messages. Connect the phone app to send one.");
        lv_obj_set_style_text_color(l, UI_COLOR_MUTED, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, LV_PCT(100));
        return;
    }

    for (size_t i = ring.size(); i-- > 0;) {
        const hal::msg::InboxEntry &m = ring[i];
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(row, 4, 0);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, UI_COLOR_MUTED, 0);
        lv_obj_set_style_border_color(row, UI_COLOR_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(row, 2, LV_STATE_FOCUSED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        lv_obj_t *title = lv_label_create(row);
        string head = (m.kind == "clip" ? string(LV_SYMBOL_PASTE " ") : string()) + m.title;
        if (m.prio == hal::msg::Prio::High) head = string(LV_SYMBOL_WARNING " ") + head;
        lv_label_set_text(title, head.c_str());
        lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(title, LV_PCT(100));

        if (!m.body.empty()) {
            lv_obj_t *body = lv_label_create(row);
            lv_label_set_text(body, m.body.c_str());
            lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(body, LV_PCT(100));
        }

        string meta = m.src;
        const string w = when_text(m.rx);
        if (!w.empty()) meta += meta.empty() ? w : "  " + w;
        if (!meta.empty()) {
            lv_obj_t *ml = lv_label_create(row);
            lv_label_set_text(ml, meta.c_str());
            lv_obj_set_style_text_color(ml, UI_COLOR_MUTED, 0);
        }

        lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_KEY, (void *)(uintptr_t)i);
        lv_group_add_obj(lv_group_get_default(), row);
    }
}

static void refresh_cb(lv_timer_t *)
{
    if (find_btn) {
        const bool can = hw_link_connected() && !ui_companion_call_busy();
        if (can) lv_obj_remove_state(find_btn, LV_STATE_DISABLED);
        else lv_obj_add_state(find_btn, LV_STATE_DISABLED);
    }
    if (hal::companion::inbox_generation() != s_gen) rebuild();
}

static void ui_inbox_enter(lv_obj_t *parent)
{
    if (menu != NULL) return;
    enable_keyboard();
    menu = create_menu(parent);
    ui_show_back_button(back_cb);

    lv_obj_t *page = lv_menu_page_create(menu, NULL);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 4, 0);

    lv_obj_t *actions = lv_obj_create(page);
    lv_obj_set_width(actions, LV_PCT(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, 6, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    find_btn = make_action(actions, LV_SYMBOL_BELL " Find phone", find_cb);
    make_action(actions, LV_SYMBOL_TRASH " Clear", clear_cb);

    list = lv_obj_create(page);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);

    rebuild();
    refresh_cb(NULL);
    refresh_timer = lv_timer_create(refresh_cb, 1000, NULL);
    lv_menu_set_page(menu, page);
    lv_group_focus_obj(find_btn);

#ifdef USING_TOUCHPAD
    quit_btn = create_floating_button(back_cb, NULL);
#endif
}

static void ui_inbox_exit()
{
    ui_hide_back_button();
    disable_keyboard();
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = NULL;
    }
    ui_app_widgets_del(&menu, &quit_btn);
    list = NULL;
    find_btn = NULL;
    s_gen = 0;
}

#include "app_registry.h"

namespace {
class InboxApp : public core::App {
public:
    InboxApp() : core::App("Inbox") {}
    void onStart(lv_obj_t *parent) override { ui_inbox_enter(parent); }
    void onStop() override
    {
        ui_inbox_exit();
        core::App::onStop();
    }
};
} // namespace

namespace apps {
int inbox_unread_count() { return hal::companion::inbox_unread(); }
APP_FACTORY(make_inbox_app, InboxApp)
} // namespace apps
