/**
 * @file      link_buf.h
 * @brief     Fixed-capacity byte buffer for the PagerLink codec, PSRAM-first on the device (P7.36).
 *
 * The link's message-sized buffers (≤ 4102 B) sit right at the ESP-IDF
 * internal-first malloc threshold, so plain std::string/vector put most of them
 * in internal RAM, where TLS needs a ~16.7 KB contiguous block (P6.30). A
 * ByteBuf allocates its storage once, explicitly: on ARDUINO through
 * heap_caps_malloc(MALLOC_CAP_SPIRAM) with an internal fallback, on the host
 * (native tests, SDL emulator) through plain malloc. Header-only so the
 * native_test build_src_filter needs no extra TU.
 *
 * Not copyable (it owns one raw block); movable.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace pagerlink {

/** Allocate `n` bytes, PSRAM first on the device; nullptr on failure. */
inline void *link_alloc(size_t n) {
#ifdef ARDUINO
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return p;
#else
    return std::malloc(n);
#endif
}

/** Release a block from link_alloc() (nullptr is fine). */
inline void link_free(void *p) {
#ifdef ARDUINO
    heap_caps_free(p);
#else
    std::free(p);
#endif
}

/** A byte buffer whose capacity is fixed at the first reserve(). */
class ByteBuf {
public:
    ByteBuf() = default;
    ~ByteBuf() { link_free(data_); }
    ByteBuf(const ByteBuf &) = delete;
    ByteBuf &operator=(const ByteBuf &) = delete;
    ByteBuf(ByteBuf &&o) noexcept : data_(o.data_), size_(o.size_), cap_(o.cap_) {
        o.data_ = nullptr;
        o.size_ = o.cap_ = 0;
    }
    ByteBuf &operator=(ByteBuf &&o) noexcept {
        if (this != &o) {
            link_free(data_);
            data_ = o.data_;
            size_ = o.size_;
            cap_ = o.cap_;
            o.data_ = nullptr;
            o.size_ = o.cap_ = 0;
        }
        return *this;
    }

    /** Allocate `cap` bytes once; a later call with cap ≤ capacity() is a no-op.
     * Growing reallocates (contents kept). False on allocation failure. */
    bool reserve(size_t cap) {
        if (cap <= cap_) {
            return true;
        }
        uint8_t *p = static_cast<uint8_t *>(link_alloc(cap));
        if (p == nullptr) {
            return false;
        }
        if (size_ != 0) {
            std::memcpy(p, data_, size_);
        }
        link_free(data_);
        data_ = p;
        cap_ = cap;
        return true;
    }
    /** Set the size; false (size unchanged) when `n` exceeds capacity(). */
    bool resize(size_t n) {
        if (n > cap_) {
            return false;
        }
        size_ = n;
        return true;
    }
    /** Append `n` bytes; false (unchanged) when they do not fit in capacity(). */
    bool append(const uint8_t *p, size_t n) {
        if (n > cap_ - size_) {
            return false;
        }
        if (n != 0) {
            std::memcpy(data_ + size_, p, n);
        }
        size_ += n;
        return true;
    }
    void clear() { size_ = 0; }

    uint8_t *data() { return data_; }
    const uint8_t *data() const { return data_; }
    size_t size() const { return size_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return size_ == 0; }

private:
    uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
};

}  // namespace pagerlink
