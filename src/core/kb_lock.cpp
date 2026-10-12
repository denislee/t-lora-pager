/**
 * @file      kb_lock.cpp
 * @brief     Implementation of core::kb_lock_init + ScopedKbLock (see kb_lock.h).
 *
 * The emulator build compiles this file too and substitutes no-op take/give
 * so `core::ScopedKbLock` is usable unconditionally from shared code.
 */
#include "kb_lock.h"

#ifdef ARDUINO

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
// Recursive: hw_enable_keyboard() takes the lock and then calls
// hw_set_kb_backlight(), which takes it again. Null until kb_lock_init().
SemaphoreHandle_t s_kb_mutex = nullptr;
}

namespace core {

void kb_lock_init()
{
    if (s_kb_mutex != nullptr) return;
    s_kb_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_kb_mutex == nullptr) {
        log_e("kb_lock_init: xSemaphoreCreateRecursiveMutex failed");
        assert(0);
    }
}

ScopedKbLock::ScopedKbLock()
{
    if (s_kb_mutex == nullptr) return;
    if (xSemaphoreTakeRecursive(s_kb_mutex, portMAX_DELAY) != pdTRUE) {
        log_e("ScopedKbLock: xSemaphoreTakeRecursive failed");
        assert(0);
    }
}

ScopedKbLock::~ScopedKbLock()
{
    if (s_kb_mutex == nullptr) return;
    if (xSemaphoreGiveRecursive(s_kb_mutex) != pdTRUE) {
        log_e("ScopedKbLock: xSemaphoreGiveRecursive failed");
        assert(0);
    }
}

} // namespace core

#else  // !ARDUINO — emulator: no keyboard bus; all ops are no-ops.

namespace core {

void kb_lock_init() {}

ScopedKbLock::ScopedKbLock() {}
ScopedKbLock::~ScopedKbLock() {}

} // namespace core

#endif  // ARDUINO
