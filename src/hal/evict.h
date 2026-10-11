/**
 * @file      evict.h
 * @brief     One step of the internal->SD note eviction in
 *            hw_prune_internal_storage.
 *
 * HAL internal — not a public API. The internal (FFat) copy is removed only
 * after the SD write reports success, because the SD copy is the only safety
 * net: a failed write must keep the original, so the source is dropped last.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace hal {

enum class EvictResult { Moved, ReadFailed, CopyFailed };

/* Read the source, write it to the destination, and only then drop the
 * source. Any failure before the write lands leaves the source untouched. */
template <class Read, class Write, class Remove>
EvictResult evict_one(Read read_src, Write write_dst, Remove remove_src)
{
    std::vector<uint8_t> buf;
    if (!read_src(buf)) return EvictResult::ReadFailed;
    if (!write_dst(buf)) return EvictResult::CopyFailed;
    remove_src();
    return EvictResult::Moved;
}

} // namespace hal
