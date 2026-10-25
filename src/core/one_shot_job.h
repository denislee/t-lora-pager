/**
 * @file      one_shot_job.h
 * @brief     `core::OneShotJob<T>` — run one blocking call on a worker thread
 *            and hand its heap result back to the LVGL thread.
 *
 * This is the {worker, done flag, heap result handed to the UI thread}
 * scaffold that the weather, city search, notes scan, internet checks,
 * Telegram and chat workers each used to hand-roll. The contract:
 *
 *  - The worker runs `work()` and touches nothing but its own locals and the
 *    heap result it returns — never the LVGL tree — so it is safe for it to
 *    outlive the app/page that started it.
 *  - The worker hands the result to the LVGL thread with `core::ui_post`
 *    (P7.47: the post wakes the display task, so delivery is immediate). Every
 *    method below runs on the LVGL thread — `start`/`teardown`/`abandon`/`busy`
 *    from LVGL callbacks, the delivery from `core::System::loop()` — so the job
 *    state needs no lock of its own.
 *  - `on_done(result)` runs on the LVGL thread; the job deletes the result once
 *    the callback returns. A null result skips the callback.
 *  - `start()` while a worker is still running does not spawn a second one: it
 *    re-attaches, so that worker's result still lands (e.g. the app was left
 *    and re-opened mid-job).
 *  - `teardown()` never kills a running worker: it detaches it, so its result
 *    is freed instead of delivered — unless a later `start()` re-attaches.
 *  - `abandon()` drops the running worker for good (its result is freed when
 *    it finishes, even if `start()` runs again), so the next `start()` spawns a
 *    fresh worker right away. For cancel buttons and watchdogs.
 *
 * The job object must have static storage duration — the worker and its
 * posted delivery hold a pointer to it.
 *
 * Hardware: the worker is a FreeRTOS task (name, stack, priority, core as
 * passed). Emulator: a detached pthread (the stack/priority/core arguments are
 * ignored), so `work()` never blocks the LVGL thread there either (DC2.0.6:
 * `hw_link_call` must not run on it). `one_shot_jobs_in_flight()` lets the
 * emulator screenshot sweep wait for workers before it captures.
 */
#pragma once

#include <functional>
#include <utility>

#include "ui_post.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <atomic>
#include <pthread.h>
#include <unistd.h>
#endif

namespace core {

#ifndef ARDUINO
// Emulator: number of OneShotJob workers whose result has not been posted yet.
inline std::atomic<int> &one_shot_jobs_in_flight()
{
    static std::atomic<int> n{0};
    return n;
}
#endif

template <typename T>
class OneShotJob {
public:
    using Work = std::function<T *()>;   // worker thread; returns a heap result (or nullptr)
    using Done = void (*)(T &result);     // LVGL thread; the job frees `result` afterwards

    enum class Start {
        Started,    // a new worker was spawned
        Attached,   // a previous worker is still running; its result will land
        Failed,     // the worker could not be created; nothing is running
    };

#ifdef ARDUINO
    using Priority = UBaseType_t;
    using CoreId   = BaseType_t;
    static constexpr CoreId kAnyCore = tskNO_AFFINITY;
#else
    using Priority = unsigned;
    using CoreId   = int;
    static constexpr CoreId kAnyCore = -1;
#endif

    OneShotJob() = default;
    explicit OneShotJob(Done cb) : done_cb_(cb) {}
    OneShotJob(const OneShotJob &) = delete;
    OneShotJob &operator=(const OneShotJob &) = delete;

    void on_done(Done cb) { done_cb_ = cb; }

    // True while a worker is running or its result is still being delivered.
    bool busy() const { return running_; }

    Start start(Work work, uint32_t stack_bytes, const char *name,
                Priority priority, CoreId core_id = kAnyCore)
    {
        if (running_) {
            detached_ = false;         // re-adopt a worker a teardown() let go of
            return Start::Attached;
        }
        detached_ = false;
        running_  = true;
        Ctx *ctx = new Ctx{this, ++gen_, std::move(work)};
        if (!spawn(ctx, stack_bytes, name, priority, core_id)) {
            running_ = false;
            delete ctx;
            return Start::Failed;
        }
        return Start::Started;
    }

    void teardown()
    {
        if (running_) detached_ = true;   // its result is freed on arrival
    }

    void abandon()
    {
        if (!running_) return;
        ++gen_;                           // the running worker's delivery is now stale
        running_  = false;
        detached_ = false;
    }

private:
    struct Ctx {
        OneShotJob *job;
        unsigned    gen;
        Work        work;
    };

    // Worker thread.
    static void run(Ctx *ctx)
    {
        OneShotJob *job = ctx->job;
        const unsigned gen = ctx->gen;
        T *res = ctx->work();
        delete ctx;
        // The post queue is bounded (32): if it is momentarily full, wait for
        // the LVGL thread to drain it rather than lose the result (busy() would
        // then never clear).
        while (!ui_post([job, gen, res]() { job->deliver(gen, res); })) {
#ifdef ARDUINO
            vTaskDelay(pdMS_TO_TICKS(20));
#else
            usleep(20 * 1000);
#endif
        }
#ifndef ARDUINO
        one_shot_jobs_in_flight().fetch_sub(1);
#endif
    }

    // LVGL thread (core::System::loop → ui_post_drain).
    void deliver(unsigned gen, T *res)
    {
        if (gen != gen_) {                // abandon()ed: nobody wants it
            delete res;
            return;
        }
        running_ = false;
        const bool drop = detached_;
        detached_ = false;
        // running_ is already false, so a callback that start()s the job again
        // spawns a fresh worker.
        if (res && !drop && done_cb_) done_cb_(*res);
        delete res;
    }

#ifdef ARDUINO
    static void trampoline(void *arg)
    {
        run(static_cast<Ctx *>(arg));
        vTaskDelete(NULL);
    }

    static bool spawn(Ctx *ctx, uint32_t stack_bytes, const char *name,
                      Priority priority, CoreId core_id)
    {
        return xTaskCreatePinnedToCore(trampoline, name, stack_bytes, ctx, priority,
                                       nullptr, core_id) == pdPASS;
    }
#else
    static void *thread_main(void *arg)
    {
        run(static_cast<Ctx *>(arg));
        return nullptr;
    }

    static bool spawn(Ctx *ctx, uint32_t, const char *, Priority, CoreId)
    {
        one_shot_jobs_in_flight().fetch_add(1);
        pthread_t th;
        if (pthread_create(&th, nullptr, thread_main, ctx) != 0) {
            one_shot_jobs_in_flight().fetch_sub(1);
            return false;
        }
        pthread_detach(th);
        return true;
    }
#endif

    bool     running_  = false;
    bool     detached_ = false;
    unsigned gen_      = 0;
    Done     done_cb_  = nullptr;
};

} // namespace core
