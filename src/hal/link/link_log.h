/**
 * @file      link_log.h
 * @brief     LINK_LOG(fmt, ...): the PagerLink log line.
 *
 * Hardware: log_i (compiled out at CORE_DEBUG_LEVEL=0, so arguments used only
 * here must not be computed into otherwise unused locals). Emulator: printf
 * with a "[link] " prefix and a newline. LINK_LOG_ENABLED tells whether the
 * arguments get evaluated, for a helper that exists only to build one.
 */
#pragma once

#ifdef ARDUINO
#include <esp32-hal-log.h>
#define LINK_LOG(...) log_i(__VA_ARGS__)
#define LINK_LOG_ENABLED (ARDUHAL_LOG_LEVEL >= ARDUHAL_LOG_LEVEL_INFO)
#else
#include <cstdio>
#define LINK_LOG(fmt, ...) printf("[link] " fmt "\n", ##__VA_ARGS__)
#define LINK_LOG_ENABLED 1
#endif
