/**
 * @file      companion.cpp
 * @brief     The phone-messaging inbox — see companion.h.
 */
#include "companion.h"

#include <atomic>
#include <mutex>
#include <string>

#include "storage.h"

namespace hal {
namespace companion {

namespace {

std::mutex s_mu;  // guards s_ring / s_loaded
std::vector<msg::InboxEntry> s_ring;
bool s_loaded = false;
std::atomic<uint32_t> s_gen{1};

/** Load the file once (caller holds s_mu). A missing file is an empty inbox. */
void ensure_loaded() {
    if (s_loaded) return;
    s_loaded = true;
    std::vector<uint8_t> buf;
    if (hw_read_preferred_bytes(kInboxPath, buf) && !buf.empty()) {
        msg::inbox_decode(std::string(buf.begin(), buf.end()), s_ring);
    }
}

/** Write the ring back (caller holds s_mu). Best effort: a failed write keeps
 * the RAM copy, which the next change retries. */
void persist() {
    const std::string text = msg::inbox_encode(s_ring);
    hw_save_preferred_bytes(kInboxPath, reinterpret_cast<const uint8_t *>(text.data()),
                            text.size());
    s_gen.fetch_add(1);
}

}  // namespace

bool inbox_add(const msg::InboxEntry &e) {
    std::lock_guard<std::mutex> lk(s_mu);
    ensure_loaded();
    if (!msg::inbox_add(s_ring, e)) return false;
    persist();
    return true;
}

std::vector<msg::InboxEntry> inbox_snapshot() {
    std::lock_guard<std::mutex> lk(s_mu);
    ensure_loaded();
    return s_ring;
}

int inbox_unread() {
    std::lock_guard<std::mutex> lk(s_mu);
    ensure_loaded();
    return static_cast<int>(msg::inbox_unread(s_ring));
}

void inbox_mark_all_read() {
    std::lock_guard<std::mutex> lk(s_mu);
    ensure_loaded();
    if (msg::inbox_unread(s_ring) == 0) return;
    for (msg::InboxEntry &x : s_ring) x.read = true;
    persist();
}

void inbox_remove(size_t index) {
    std::lock_guard<std::mutex> lk(s_mu);
    ensure_loaded();
    if (index >= s_ring.size()) return;
    s_ring.erase(s_ring.begin() + static_cast<std::ptrdiff_t>(index));
    persist();
}

void inbox_clear() {
    std::lock_guard<std::mutex> lk(s_mu);
    ensure_loaded();
    if (s_ring.empty()) return;
    s_ring.clear();
    persist();
}

uint32_t inbox_generation() { return s_gen.load(); }

}  // namespace companion
}  // namespace hal
