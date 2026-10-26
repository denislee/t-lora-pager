/**
 * @file      instance_lock.cpp
 * @brief     Backing store for the instance (task-coordination) mutex.
 *
 * Lives here (not in factory.ino) so the .ino file stays a thin Arduino
 * shell and the mutex — real, production-grade state — sits in a regular
 * C++ TU that's easy to test and move. The emulator build compiles this
 * file too and substitutes no-op take/give so `core::ScopedInstanceLock`
 * is usable unconditionally from shared code.
 */
#include "system_hooks.h"
#include "input_profile.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t s_instance_mutex = nullptr;
}

namespace core {

void instance_lock_init()
{
    if (s_instance_mutex != nullptr) return;
    s_instance_mutex = xSemaphoreCreateMutex();
    if (s_instance_mutex == nullptr) {
        log_e("instance_lock_init: xSemaphoreCreateMutex failed");
        assert(0);
    }
}

} // namespace core

void instanceLockTake()
{
    if (s_instance_mutex == nullptr) return;
    INPUT_PROF_T0(t0);
    if (xSemaphoreTake(s_instance_mutex, portMAX_DELAY) != pdTRUE) {
        log_e("instanceLockTake: xSemaphoreTake failed");
        assert(0);
    }
    INPUT_PROF(core::prof::lock_acquired(core::prof::now_us() - t0));
}

void instanceLockGive()
{
    if (s_instance_mutex == nullptr) return;
    INPUT_PROF(core::prof::lock_released());
    if (xSemaphoreGive(s_instance_mutex) != pdTRUE) {
        log_e("instanceLockGive: xSemaphoreGive failed");
        assert(0);
    }
}

#else  // !ARDUINO — emulator: take/give are no-ops.

namespace core {
void instance_lock_init() {}
} // namespace core

void instanceLockTake() {}
void instanceLockGive() {}

#endif  // ARDUINO
