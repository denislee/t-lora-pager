/**
 * @file      crc_cache.cpp
 * @brief     CrcCache (crc_cache.h): LRU of whole-file CRC-32s, pure logic.
 */
#include "hal/link/crc_cache.h"

namespace pagerlink {

bool CrcCache::lookup(const CrcKey &k, uint32_t &crc) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        const CrcKey &e = it->key;
        if (e.vol != k.vol || e.path != k.path) continue;
        if (e.size != k.size || e.mtime != k.mtime || e.gen != k.gen) return false;
        crc = it->crc;
        entries_.splice(entries_.begin(), entries_, it);
        return true;
    }
    return false;
}

void CrcCache::store(const CrcKey &k, uint32_t crc) {
    if (cap_ == 0) return;
    evict(k.vol, k.path);
    entries_.push_front(Entry{k, crc});
    while (entries_.size() > cap_) entries_.pop_back();
}

void CrcCache::evict(uint8_t vol, const std::string &path) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->key.vol == vol && it->key.path == path) {
            entries_.erase(it);
            return;
        }
    }
}

}  // namespace pagerlink
