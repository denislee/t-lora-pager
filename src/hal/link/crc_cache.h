/**
 * @file      crc_cache.h
 * @brief     Small LRU of whole-file CRC-32s for the PagerLink file service.
 *
 * Pure logic, no HAL and no locking (fs_volume.cpp wraps it in a mutex and
 * feeds it the keys). An entry is a hit only when every field of the key
 * matches: the same volume and path, the same size and mtime from a fresh
 * stat, and the same filesystem generation (hw_get_filesystem_generation(),
 * bumped by every write that goes through the storage HAL). Any write the
 * storage HAL sees therefore invalidates the whole cache; size + mtime catch
 * most writers that bypass it. One entry per (vol, path): storing a new key for
 * a path replaces its old entry.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>

namespace pagerlink {

struct CrcKey {
    uint8_t vol;      /* pagerlink::Vol as an integer */
    std::string path; /* sandboxed absolute path */
    uint32_t size;
    uint32_t mtime;
    uint32_t gen;     /* filesystem generation when the CRC was computed */
};

class CrcCache {
public:
    explicit CrcCache(size_t capacity) : cap_(capacity) {}

    /** True and `crc` set when an entry matches every field of `k`; a hit becomes most recent. */
    bool lookup(const CrcKey &k, uint32_t &crc);
    /** Insert or replace the entry for (k.vol, k.path); evicts the least recent past capacity. */
    void store(const CrcKey &k, uint32_t crc);
    /** Forget (vol, path), whatever its size/mtime/generation. */
    void evict(uint8_t vol, const std::string &path);
    void clear() { entries_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        CrcKey key;
        uint32_t crc;
    };
    size_t cap_;
    std::list<Entry> entries_; /* most recent first */
};

}  // namespace pagerlink
