/**
 * @file      notify.cpp
 */
#include "notify.h"

#include <lvgl.h>

#include <deque>
#include <mutex>
#include <vector>

#include "../ui_define.h"
#include "system_hooks.h"  // hw_lvgl_task_notify_wake (P6.18 / P7.47)

#ifdef ARDUINO
#include "../hal_interface.h"  // hw_feedback
#endif

namespace core {
namespace notify {

namespace {

std::mutex              s_mu;              // guards s_queue + s_next_id
std::deque<Notification> s_queue;
uint32_t                s_next_id = 1;

std::vector<Subscriber> s_subs;            // LVGL-thread only

// --- Default renderer: banner stack on lv_layer_top() --------------------

struct ActiveBanner {
    uint32_t id;
    lv_obj_t *obj;     // the banner container; nullptr once dismissed
    lv_timer_t *timer; // one-shot dismiss timer; nullptr for sticky
};
static std::vector<ActiveBanner> s_active;  // LVGL-thread only
static bool s_renderer_installed = false;

constexpr int32_t kBannerGap   = 4;
constexpr int32_t kBannerPadX  = 8;
constexpr int32_t kBannerPadY  = 4;
constexpr size_t  kMaxVisible  = 3;

static lv_color_t sev_bg(Severity s)
{
    switch (s) {
    case Severity::Success: return lv_palette_darken(LV_PALETTE_GREEN, 2);
    case Severity::Warning: return lv_palette_darken(LV_PALETTE_AMBER, 2);
    case Severity::Error:   return lv_palette_darken(LV_PALETTE_RED,   2);
    case Severity::Info:
    default:                return lv_color_hex(0x0b6fa8);
    }
}

static void restack_banners()
{
    int32_t y = kBannerGap;
    for (auto &b : s_active) {
        if (!b.obj) continue;
        lv_obj_align(b.obj, LV_ALIGN_TOP_MID, 0, y);
        y += lv_obj_get_height(b.obj) + kBannerGap;
    }
}

static void remove_active(uint32_t id, bool delete_timer)
{
    for (auto it = s_active.begin(); it != s_active.end(); ++it) {
        if (it->id != id) continue;
        if (it->obj)   lv_obj_del(it->obj);
        if (delete_timer && it->timer) lv_timer_del(it->timer);
        s_active.erase(it);
        restack_banners();
        return;
    }
}

// lv_timer_set_repeat_count(t, 1) means LVGL frees the timer itself after
// it fires — we must only remove the banner, not the timer.
static void banner_timeout_cb(lv_timer_t *t)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_timer_get_user_data(t);
    remove_active(id, /*delete_timer=*/false);
}

static void default_renderer(const Notification &n)
{
    if (s_active.size() >= kMaxVisible) {
        // Evict the oldest so new high-priority events always show.
        if (s_active.front().obj)   lv_obj_del(s_active.front().obj);
        if (s_active.front().timer) lv_timer_del(s_active.front().timer);
        s_active.erase(s_active.begin());
    }

    lv_obj_t *top = lv_layer_top();
    lv_obj_t *banner = lv_obj_create(top);
    lv_obj_remove_style_all(banner);
    lv_obj_set_size(banner, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(banner, sev_bg(n.severity), 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(banner, 6, 0);
    lv_obj_set_style_pad_hor(banner, kBannerPadX, 0);
    lv_obj_set_style_pad_ver(banner, kBannerPadY, 0);
    lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);

    if (!n.title.empty() || n.icon) {
        lv_obj_t *t = lv_label_create(banner);
        std::string s;
        if (n.icon) { s += n.icon; s += "  "; }
        s += n.title;
        lv_label_set_text(t, s.c_str());
        lv_obj_set_style_text_color(t, UI_COLOR_ON_ACCENT, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, LV_PCT(100));
    }
    if (!n.body.empty()) {
        lv_obj_t *b = lv_label_create(banner);
        lv_label_set_text(b, n.body.c_str());
        lv_obj_set_style_text_color(b, UI_COLOR_ON_ACCENT, 0);
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        lv_obj_set_width(b, LV_PCT(100));
    }

    ActiveBanner ab{};
    ab.id    = n.id;
    ab.obj   = banner;
    ab.timer = nullptr;
    if (n.duration_ms > 0) {
        ab.timer = lv_timer_create(banner_timeout_cb, n.duration_ms,
                                   (void *)(uintptr_t)n.id);
        lv_timer_set_repeat_count(ab.timer, 1);
    }
    s_active.push_back(ab);
    restack_banners();

#ifdef ARDUINO
    if (n.haptic) hw_feedback();
#else
    (void)n.haptic;
#endif
}

} // namespace

uint32_t post(Notification n)
{
    uint32_t id;
    {
        std::lock_guard<std::mutex> g(s_mu);
        id = s_next_id++;
        if (s_next_id == 0) s_next_id = 1;  // skip 0, it means "invalid"
        n.id = id;
        s_queue.push_back(std::move(n));
    }
    // pump() runs on every LVGL task pass; wake it so the banner doesn't wait
    // for the next timer deadline (up to 200 ms on a static screen).
    hw_lvgl_task_notify_wake();
    return id;
}

void subscribe(Subscriber s)
{
    s_subs.push_back(std::move(s));
}

void pump()
{
    // Pull pending items under the lock, then dispatch without holding it
    // so subscribers (which may allocate, touch LVGL, etc.) don't serialize
    // against `post()` from other tasks.
    // Fast path: nothing queued. Checked under the lock but before allocating
    // any batch container, so the idle ~60 Hz tick does zero heap work.
    // (libstdc++ default-constructs std::deque with a live allocation.) A post()
    // racing in right after we return here is simply picked up next tick.
    {
        std::lock_guard<std::mutex> g(s_mu);
        if (s_queue.empty()) return;
    }

    std::deque<Notification> batch;
    {
        std::lock_guard<std::mutex> g(s_mu);
        batch.swap(s_queue);
    }
    for (auto &n : batch) {
        for (auto &sub : s_subs) sub(n);
    }
}

void install_default_renderer()
{
    if (s_renderer_installed) return;
    s_renderer_installed = true;
    subscribe(default_renderer);
}

} // namespace notify
} // namespace core
