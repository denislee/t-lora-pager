/**
 * @file      link_log.h
 * @brief     LINK_LOG(fmt, ...): the PagerLink log line.
 *
 * Hardware: log_i (compiled out at CORE_DEBUG_LEVEL=0, so arguments used only
 * here must not be computed into otherwise unused locals). Emulator: printf
 * with a "[link] " prefix and a newline.
 */
#pragma once

#ifdef ARDUINO
#include <esp32-hal-log.h>
#define LINK_LOG(...) log_i(__VA_ARGS__)
#else
#include <cstdio>
#define LINK_LOG(fmt, ...) printf("[link] " fmt "\n", ##__VA_ARGS__)
#endif
