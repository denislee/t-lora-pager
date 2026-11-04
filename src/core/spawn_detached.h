/**
 * @file      spawn_detached.h
 * @brief     `core::spawn_detached` — run a closure on its own fire-and-forget
 *            worker: a FreeRTOS task on hardware, a detached pthread on the
 *            emulator (where stack, priority and core are ignored).
 *
 * False means no worker exists and `fn` will never run (it has been destroyed
 * by then, along with its captures). The caller still owes whoever is waiting
 * for the work an answer. For a worker whose result returns to the LVGL thread,
 * use core::OneShotJob instead.
 */
#pragma once

#include <cstdint>
#include <functional>

namespace core {

constexpr int kSpawnAnyCore = -1;

bool spawn_detached(const char *name, uint32_t stack_bytes, std::function<void()> fn,
                    unsigned priority = 1, int core_id = kSpawnAnyCore);

}  // namespace core
