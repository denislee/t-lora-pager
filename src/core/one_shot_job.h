/**
 * @file      one_shot_job.h
 * @brief     `core::OneShotJob<T>` — run one blocking call on a FreeRTOS
 *            worker and hand its heap result back to the LVGL thread.
 *
 * This is the {task handle, 100 ms drain timer, volatile done flag, heap
 * result swapped under the instance lock} scaffold that the weather, city
 * search, notes scan, internet checks and Telegram favourites each used to
 * hand-roll. The contract every one of those copies relied on:
 *
 *  - The worker runs `work()` and touches nothing but its own locals and the
 *    heap result it returns — never the LVGL tree — so it is safe for it to
 *    outlive the app/page that started it.
 *  - The result is published under `core::ScopedInstanceLock`. Every method
 *    below runs on the LVGL thread inside `lv_timer_handler()`, which already
 *    holds that (non-recursive) lock, so none of them re-take it — doing so
 *    would self-deadlock.
 *  - `on_done(result)` runs on the LVGL thread from a 100 ms drain timer. The
 *    job deletes the result once the callback returns. A null result skips
 *    the callback.
 *  - `start()` while a worker is still running does not spawn a second one: it
 *    re-attaches the drain so that worker's result still lands (e.g. the app
 *    was left and re-opened mid-job).
 *  - `teardown()` stops the drain but never kills a running worker: it
 *    detaches it, so the worker frees its own result when it finishes instead
 *    of publishing it. A completed-but-undrained result is freed right away.
 *
 * The job object must have static storage duration — the worker holds a
 * pointer to it for as long as it runs.
 *
 * Emulator (no FreeRTOS): `start()` runs `work()` inline and delivers
 * `on_done` synchronously before returning `Started`.
 */
#pragma once

#include <lvgl.h>
#include <functional>
#include <utility>

#include "scoped_lock.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace core {

template <typename T>
class OneShotJob {
public:
    using Work = std::function<T *()>;   // worker thread; returns a heap result (or nullptr)
    using Done = void (*)(T &result);     // LVGL thread; the job frees `result` afterwards

    enum class Start {
        Started,    // a new worker was spawned (or, on the emulator, ran inline)
        Attached,   // a previous worker is still running; its result will land
        Failed,     // the task could not be created; nothing is running
    };

    OneShotJob() = default;
    explicit OneShotJob(Done cb) : done_cb_(cb) {}
    OneShotJob(const OneShotJob &) = delete;
    OneShotJob &operator=(const OneShotJob &) = delete;

    void on_done(Done cb) { done_cb_ = cb; }

#ifdef ARDUINO
    // True while a worker is running or its drain is still pending.
    bool busy() const { return task_ != nullptr || timer_ != nullptr; }

    Start start(Work work, uint32_t stack_bytes, const char *name,
                UBaseType_t priority, BaseType_t core_id = tskNO_AFFINITY)
    {
        if (task_ != nullptr) {
            detached_ = false;         // re-adopt a worker a teardown() let go of
            ensure_timer();
            return Start::Attached;
        }
        done_ = false;
        detached_ = false;
        if (result_) { delete result_; result_ = nullptr; }

        Ctx *ctx = new Ctx{this, std::move(work)};
        if (xTaskCreatePinnedToCore(trampoline, name, stack_bytes, ctx, priority,
                                    &task_, core_id) != pdPASS) {
            task_ = nullptr;
            delete ctx;
            return Start::Failed;
        }
        ensure_timer();
        return Start::Started;
    }

    void teardown()
    {
        if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
        if (done_) {
            done_ = false;
            if (result_) { delete result_; result_ = nullptr; }
        } else if (task_ != nullptr) {
            detached_ = true;          // the worker frees its result on finish
        }
    }
#else
    bool busy() const { return false; }

    Start start(Work work, uint32_t stack_bytes, const char *name,
                unsigned priority, int core_id = -1)
    {
        (void)stack_bytes; (void)name; (void)priority; (void)core_id;
        T *res = work();
        if (res) {
            if (done_cb_) done_cb_(*res);
            delete res;
        }
        return Start::Started;
    }

    void teardown() {}
#endif

private:
#ifdef ARDUINO
    struct Ctx {
        OneShotJob *job;
        Work        work;
    };

    static void trampoline(void *arg)
    {
        Ctx *ctx = static_cast<Ctx *>(arg);
        OneShotJob *job = ctx->job;
        T *res = ctx->work();
        delete ctx;
        {
            ScopedInstanceLock lock;
            if (job->detached_) {
                delete res;            // nobody is listening any more
            } else {
                if (job->result_) delete job->result_;
                job->result_ = res;
                job->done_ = true;
            }
            job->detached_ = false;
            job->task_ = nullptr;
        }
        vTaskDelete(NULL);
    }

    static void drain_tick(lv_timer_t *t)
    {
        OneShotJob *job = static_cast<OneShotJob *>(lv_timer_get_user_data(t));
        if (!job->done_) return;
        job->done_ = false;
        T *res = job->result_;
        job->result_ = nullptr;
        // Drop the timer before the callback so a callback that start()s the
        // job again gets a fresh drain instead of having it deleted under it.
        if (job->timer_) { lv_timer_delete(job->timer_); job->timer_ = nullptr; }
        if (!res) return;
        if (job->done_cb_) job->done_cb_(*res);
        delete res;
    }

    void ensure_timer()
    {
        if (!timer_) timer_ = lv_timer_create(drain_tick, 100, this);
    }

    TaskHandle_t  task_     = nullptr;
    lv_timer_t   *timer_    = nullptr;
    volatile bool done_     = false;
    bool          detached_ = false;   // guarded by the instance lock
    T            *result_   = nullptr; // guarded by the instance lock
#endif
    Done          done_cb_  = nullptr;
};

} // namespace core
