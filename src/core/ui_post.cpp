/**
 * @file      ui_post.cpp
 * @brief     ui_post_* — see ui_post.h.
 */
#include "ui_post.h"

#include <deque>
#include <mutex>

#include "system_hooks.h"  // hw_lvgl_task_notify_wake (P7.47)

namespace core {

namespace {

std::mutex                        s_mu;      // guards s_queue
std::deque<std::function<void()>> s_queue;
constexpr size_t                  kMaxPending = 32;

} // namespace

bool ui_post(std::function<void()> fn)
{
    if (!fn) return false;
    {
        std::lock_guard<std::mutex> g(s_mu);
        if (s_queue.size() >= kMaxPending) return false;   // bounded: drop, don't grow
        s_queue.push_back(std::move(fn));
    }
    // Wake the display task so the closure doesn't wait for the next timer
    // deadline (up to 200 ms on a static screen).
    hw_lvgl_task_notify_wake();
    return true;
}

void ui_post_drain()
{
    // Swap the batch out under the lock, then run it without the lock: a
    // closure may post again (that lands in the next drain, and has already
    // woken the task) or block on something a producer would then be stuck
    // behind.
    std::deque<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> g(s_mu);
        if (s_queue.empty()) return;
        batch.swap(s_queue);
    }
    for (auto &fn : batch) {
        if (fn) fn();
    }
}

} // namespace core
