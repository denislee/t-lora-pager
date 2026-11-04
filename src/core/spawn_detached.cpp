/**
 * @file      spawn_detached.cpp
 */
#include "spawn_detached.h"

#include <memory>
#include <utility>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <pthread.h>
#endif

namespace core {

namespace {

using Closure = std::function<void()>;

// Frees the closure before returning: vTaskDelete never returns to its caller.
void run_closure(void *arg)
{
    std::unique_ptr<Closure> fn(static_cast<Closure *>(arg));
    (*fn)();
}

#ifdef ARDUINO
void task_main(void *arg)
{
    run_closure(arg);
    vTaskDelete(nullptr);
}
#else
void *thread_main(void *arg)
{
    run_closure(arg);
    return nullptr;
}
#endif

}  // namespace

bool spawn_detached(const char *name, uint32_t stack_bytes, std::function<void()> fn,
                    unsigned priority, int core_id)
{
    Closure *heap = new Closure(std::move(fn));
#ifdef ARDUINO
    const BaseType_t core = core_id == kSpawnAnyCore ? static_cast<BaseType_t>(tskNO_AFFINITY)
                                                     : static_cast<BaseType_t>(core_id);
    if (xTaskCreatePinnedToCore(task_main, name, stack_bytes, heap, priority, nullptr, core) != pdPASS) {
        delete heap;
        return false;
    }
#else
    (void)name;
    (void)stack_bytes;
    (void)priority;
    (void)core_id;
    pthread_t th;
    if (pthread_create(&th, nullptr, thread_main, heap) != 0) {
        delete heap;
        return false;
    }
    pthread_detach(th);
#endif
    return true;
}

}  // namespace core
