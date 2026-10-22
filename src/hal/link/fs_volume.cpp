/**
 * @file      fs_volume.cpp
 * @brief     Raw volume I/O for the PagerLink file service (fs_volume.h).
 *
 * Implements the byte-level filesystem primitives behind handlers_fs.cpp. On
 * hardware the internal volume is FFat and the SD card is reached through the
 * vendor SPI bus (every access holds a core::ScopedSpiLock); on the emulator
 * both volumes are real directories under $PAGERLINK_FS_ROOT. No crypto and no
 * path sandboxing live here — callers pass already-sanitised absolute paths.
 */
#include "hal/link/fs_volume.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "hal/link/crc_cache.h"
#include "hal/link/link_xfer.h"
#include "hal/storage.h"

#ifdef ARDUINO
#include <FFat.h>
#include <LilyGoLib.h>
#include <SD.h>

#include <memory>

#include "../../core/spi_lock.h"
#include "../storage.h"
#include "../system.h"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#endif

namespace pagerlink {

namespace {
/* CRC cache invalidation for writes that go through this layer (defined at the
 * bottom): the storage-HAL generation does not see them. */
void crc_forget_path(Vol v, const std::string &path);
void crc_forget_all();
}  // namespace

bool vol_from_string(const std::string &s, Vol &out) {
    if (s == "int") {
        out = Vol::Int;
        return true;
    }
    if (s == "sd") {
        out = Vol::Sd;
        return true;
    }
    return false;
}

#ifdef ARDUINO

namespace {

/** Leaf component of a path or a vendor entry.name() (SD reports full paths). */
std::string leaf_name(const char *name) {
    const std::string s = name ? name : "";
    const size_t slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

/** Holds the SPI bus only while the SD volume is in use; a no-op for FFat. */
struct VolLock {
    std::unique_ptr<core::ScopedSpiLock> l;
    explicit VolLock(Vol v) {
        if (v == Vol::Sd) l.reset(new core::ScopedSpiLock());
    }
};

fs::FS &fs_for(Vol v) {
    return v == Vol::Sd ? static_cast<fs::FS &>(SD) : static_cast<fs::FS &>(FFat);
}

/** Orders a listing: folders first, then files, each by leaf name. */
void append_sorted(std::vector<VolEntry> &out, std::vector<VolEntry> dirs,
                   std::vector<VolEntry> files) {
    auto by_name = [](const VolEntry &a, const VolEntry &b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    out = std::move(dirs);
    out.insert(out.end(), files.begin(), files.end());
}

/** Recursive delete; the caller holds the volume lock for the whole walk. */
bool remove_recursive(fs::FS &fs, const std::string &path) {
    File f = fs.open(path.c_str());
    if (!f) return false;
    if (!f.isDirectory()) {
        f.close();
        return fs.remove(path.c_str());
    }

    std::vector<std::string> children;
    File entry = f.openNextFile();
    while (entry) {
        children.push_back(path + "/" + leaf_name(entry.name()));
        entry.close();
        entry = f.openNextFile();
    }
    f.close();

    for (const std::string &c : children) {
        if (!remove_recursive(fs, c)) return false;
    }
    return fs.rmdir(path.c_str());
}

}  // namespace

bool vol_available(Vol v) {
    if (v == Vol::Int) return true;
    return (HW_SD_ONLINE & hw_get_device_online()) != 0;
}

bool vol_list(Vol v, const std::string &dir, std::vector<VolEntry> &out) {
    out.clear();
    if (!vol_available(v)) return false;

    VolLock lock(v);
    fs::FS &fs = fs_for(v);
    File d = fs.open(dir.c_str());
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return false;
    }

    std::vector<VolEntry> dirs;
    std::vector<VolEntry> files;
    File entry = d.openNextFile();
    while (entry) {
        const bool is_dir = entry.isDirectory();
        VolEntry e;
        e.name = leaf_name(entry.name());
        e.dir = is_dir;
        e.size = is_dir ? 0u : static_cast<uint32_t>(entry.size());
        // Skip entry.getLastWrite(): a slow f_stat lookup on FAT. mtime is 0.
        e.mtime = 0;
        (is_dir ? dirs : files).push_back(e);
        entry.close();
        entry = d.openNextFile();
    }
    d.close();

    append_sorted(out, std::move(dirs), std::move(files));
    return true;
}

bool vol_stat(Vol v, const std::string &path, VolEntry &out) {
    if (!vol_available(v)) return false;
    if (path == "/") {
        out = VolEntry{"", true, 0, 0};
        return true;
    }

    VolLock lock(v);
    File f = fs_for(v).open(path.c_str());
    if (!f) return false;

    const bool dir = f.isDirectory();
    out.name = leaf_name(path.c_str());
    out.dir = dir;
    out.size = dir ? 0u : static_cast<uint32_t>(f.size());
    out.mtime = static_cast<uint32_t>(f.getLastWrite());
    f.close();
    return true;
}

bool vol_read_at(Vol v, const std::string &path, uint32_t offset, uint8_t *buf, size_t n) {
    if (!vol_available(v)) return false;

    VolLock lock(v);
    File f = fs_for(v).open(path.c_str(), FILE_READ);
    if (!f) return false;
    if (f.isDirectory()) {
        f.close();
        return false;
    }
    if (f.size() < static_cast<uint64_t>(offset) + n) {
        f.close();
        return false;
    }
    if (!f.seek(offset)) {
        f.close();
        return false;
    }

    size_t got = 0;
    while (got < n) {
        const int r = f.read(buf + got, n - got);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    f.close();
    return got == n;
}

bool vol_write_at(Vol v, const std::string &path, uint32_t offset, const uint8_t *buf, size_t n) {
    if (!vol_available(v)) return false;
    crc_forget_path(v, path);

    VolLock lock(v);
    fs::FS &fs = fs_for(v);

    File f;
    if (offset == 0) {
        f = fs.open(path.c_str(), FILE_WRITE);  // create + truncate
    } else {
        f = fs.open(path.c_str(), FILE_APPEND);
        if (!f) return false;
        if (static_cast<uint32_t>(f.size()) != offset) {
            f.close();
            return false;
        }
    }
    if (!f) return false;

    const size_t wrote = n ? f.write(buf, n) : 0;
    f.close();
    return wrote == n;
}

bool vol_remove(Vol v, const std::string &path) {
    if (!vol_available(v)) return false;
    if (path.empty() || path == "/") return false;
    crc_forget_all();

    VolLock lock(v);
    return remove_recursive(fs_for(v), path);
}

bool vol_mkdir(Vol v, const std::string &path) {
    if (!vol_available(v)) return false;

    VolLock lock(v);
    fs::FS &fs = fs_for(v);
    File f = fs.open(path.c_str());
    if (f) {
        const bool dir = f.isDirectory();
        f.close();
        return dir;
    }
    return fs.mkdir(path.c_str());
}

bool vol_rename(Vol v, const std::string &from, const std::string &to) {
    if (!vol_available(v)) return false;
    crc_forget_all();

    VolLock lock(v);
    fs::FS &fs = fs_for(v);
    if (fs.exists(to.c_str())) return false;
    return fs.rename(from.c_str(), to.c_str());
}

bool vol_free_bytes(Vol v, uint64_t &out) {
    if (!vol_available(v)) return false;

    if (v == Vol::Int) {
        out = FFat.totalBytes() - FFat.usedBytes();
        return true;
    }

    VolLock lock(v);
    out = SD.totalBytes() - SD.usedBytes();
    return true;
}

// --------------------------------------------------------------------------
// VolFile (hardware)
// --------------------------------------------------------------------------

namespace {
/* A kept-open write syncs (fflush + fsync: size, FAT, directory entry) every
 * this many bytes, so a crash mid-upload leaks at most this much allocation;
 * close() syncs the rest. Was: a sync per 4092 B chunk. */
constexpr uint32_t SYNC_EVERY = 64 * 1024;

/* FFat stays mounted for the whole run. The SD card does not: the vendor
 * lightSleep() calls SD.end() and SD.begin() again, and nothing tells us, so a
 * kept FILE* could outlive its mount and its fd be reused by a file opened
 * after the remount. SD therefore reopens per chunk, as before P7.34. */
bool keep_open(Vol v) { return v == Vol::Int; }
}  // namespace

struct VolFile::Impl {
    Vol v = Vol::Int;
    std::string path;
    File f;
    bool open = false;
    uint32_t pos = 0;      /* read: stream position; write: bytes in the file */
    uint32_t unsynced = 0; /* write: bytes since the last sync */

    /* Caller holds the VolLock. */
    void close_locked() {
        if (!open) return;
        f.close(); /* syncs a write */
        open = false;
        unsynced = 0;
    }
};

VolFile::VolFile() : impl_(new Impl) {}

VolFile::~VolFile() { close(); }

void VolFile::close() {
    if (!impl_->open) return;
    VolLock lock(impl_->v);
    impl_->close_locked();
}

bool VolFile::open_read(Vol v, const std::string &path) {
    close();
    impl_->v = v;
    impl_->path = path;
    if (!vol_available(v)) return false;

    VolLock lock(v);
    File f = fs_for(v).open(path.c_str(), FILE_READ);
    if (!f) return false;
    if (f.isDirectory()) {
        f.close();
        return false;
    }
    if (!keep_open(v)) {
        f.close();
        return true;
    }
    impl_->f = f;
    impl_->open = true;
    impl_->pos = 0;
    return true;
}

bool VolFile::open_write(Vol v, const std::string &path) {
    close();
    impl_->v = v;
    impl_->path = path;
    if (!vol_available(v)) return false;
    crc_forget_path(v, path);

    VolLock lock(v);
    File f = fs_for(v).open(path.c_str(), FILE_WRITE); /* create + truncate */
    if (!f) return false;
    if (!keep_open(v)) {
        f.close();
        return true;
    }
    impl_->f = f;
    impl_->open = true;
    impl_->pos = 0;
    impl_->unsynced = 0;
    return true;
}

bool VolFile::read_at(uint32_t offset, uint8_t *buf, size_t n) {
    Impl &m = *impl_;
    if (!vol_available(m.v)) return false;

    VolLock lock(m.v);
    if (!m.open) {
        m.f = fs_for(m.v).open(m.path.c_str(), FILE_READ);
        if (!m.f) return false;
        m.open = true;
        m.pos = 0;
        if (m.f.isDirectory()) {
            m.close_locked();
            return false;
        }
    }

    bool ok = true;
    if (m.pos != offset) {
        ok = m.f.seek(offset);
        if (ok) m.pos = offset;
    }
    size_t got = 0;
    while (ok && got < n) {
        const int r = m.f.read(buf + got, n - got);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    m.pos += static_cast<uint32_t>(got);
    ok = ok && got == n;
    if (!ok || !keep_open(m.v)) m.close_locked(); /* unknown position after a failure */
    return ok;
}

bool VolFile::write_at(uint32_t offset, const uint8_t *buf, size_t n) {
    Impl &m = *impl_;
    if (!vol_available(m.v)) return false;
    crc_forget_path(m.v, m.path);

    VolLock lock(m.v);
    if (!m.open) {
        m.f = fs_for(m.v).open(m.path.c_str(), FILE_APPEND);
        if (!m.f) return false;
        m.open = true;
        m.unsynced = 0;
        m.pos = static_cast<uint32_t>(m.f.size());
    }
    if (m.pos != offset) {
        m.close_locked();
        return false;
    }

    const size_t wrote = n ? m.f.write(buf, n) : 0;
    m.pos += static_cast<uint32_t>(wrote);
    m.unsynced += static_cast<uint32_t>(wrote);
    const bool ok = wrote == n;
    if (!ok || !keep_open(m.v)) {
        m.close_locked();
    } else if (m.unsynced >= SYNC_EVERY) {
        m.f.flush(); /* fflush + fsync */
        m.unsynced = 0;
    }
    return ok;
}

#else  // !ARDUINO — emulator: two real directories on the host.

namespace {

/** Leaf component of an absolute path. */
std::string leaf(const std::string &path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/** The backing root, created once: $PAGERLINK_FS_ROOT or /tmp/pagerlink-fs. */
const std::string &root() {
    static const std::string r = [] {
        const char *env = std::getenv("PAGERLINK_FS_ROOT");
        const std::string base = (env && env[0]) ? env : "/tmp/pagerlink-fs";
        ::mkdir(base.c_str(), 0755);  // ignore EEXIST / errors
        ::mkdir((base + "/int").c_str(), 0755);
        ::mkdir((base + "/sd").c_str(), 0755);
        return base;
    }();
    return r;
}

/** Map a volume + sandboxed path to a host path. */
std::string host(Vol v, const std::string &path) {
    std::string p = root();
    p += (v == Vol::Int) ? "/int" : "/sd";
    if (path != "/") p += path;
    return p;
}

/** Orders a listing: folders first, then files, each by leaf name. */
void append_sorted(std::vector<VolEntry> &out, std::vector<VolEntry> dirs,
                   std::vector<VolEntry> files) {
    auto by_name = [](const VolEntry &a, const VolEntry &b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    out = std::move(dirs);
    out.insert(out.end(), files.begin(), files.end());
}

/** Recursive delete of a host path. */
bool remove_recursive(const std::string &p) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return ::unlink(p.c_str()) == 0;

    DIR *d = ::opendir(p.c_str());
    if (!d) return false;
    struct dirent *de;
    while ((de = ::readdir(d)) != nullptr) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (!remove_recursive(p + "/" + name)) {
            ::closedir(d);
            return false;
        }
    }
    ::closedir(d);
    return ::rmdir(p.c_str()) == 0;
}

}  // namespace

bool vol_available(Vol v) {
    (void)v;
    return true;
}

bool vol_list(Vol v, const std::string &dir, std::vector<VolEntry> &out) {
    out.clear();
    DIR *d = ::opendir(host(v, dir).c_str());
    if (!d) return false;

    const std::string base = host(v, dir);
    std::vector<VolEntry> dirs;
    std::vector<VolEntry> files;
    struct dirent *de;
    while ((de = ::readdir(d)) != nullptr) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") continue;

        struct stat st;
        if (::stat((base + "/" + name).c_str(), &st) != 0) continue;

        VolEntry e;
        e.name = name;
        e.dir = S_ISDIR(st.st_mode);
        e.size = e.dir ? 0u : static_cast<uint32_t>(st.st_size);
        e.mtime = static_cast<uint32_t>(st.st_mtime);
        (e.dir ? dirs : files).push_back(e);
    }
    ::closedir(d);

    append_sorted(out, std::move(dirs), std::move(files));
    return true;
}

bool vol_stat(Vol v, const std::string &path, VolEntry &out) {
    if (path == "/") {
        out = VolEntry{"", true, 0, 0};
        return true;
    }

    struct stat st;
    if (::stat(host(v, path).c_str(), &st) != 0) return false;

    out.name = leaf(path);
    out.dir = S_ISDIR(st.st_mode);
    out.size = out.dir ? 0u : static_cast<uint32_t>(st.st_size);
    out.mtime = static_cast<uint32_t>(st.st_mtime);
    return true;
}

bool vol_read_at(Vol v, const std::string &path, uint32_t offset, uint8_t *buf, size_t n) {
    const std::string h = host(v, path);
    struct stat st;
    if (::stat(h.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) return false;
    if (static_cast<uint64_t>(st.st_size) < static_cast<uint64_t>(offset) + n) return false;

    FILE *f = std::fopen(h.c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }

    size_t got = 0;
    while (got < n) {
        const size_t r = std::fread(buf + got, 1, n - got, f);
        if (r == 0) break;
        got += r;
    }
    std::fclose(f);
    return got == n;
}

bool vol_write_at(Vol v, const std::string &path, uint32_t offset, const uint8_t *buf, size_t n) {
    crc_forget_path(v, path);
    const std::string h = host(v, path);

    const char *mode = "wb";  // offset 0: create + truncate
    if (offset != 0) {
        struct stat st;
        if (::stat(h.c_str(), &st) != 0) return false;
        if (static_cast<uint64_t>(st.st_size) != offset) return false;
        mode = "ab";
    }

    FILE *f = std::fopen(h.c_str(), mode);
    if (!f) return false;
    const size_t wrote = n ? std::fwrite(buf, 1, n, f) : 0;
    std::fclose(f);
    return wrote == n;
}

bool vol_remove(Vol v, const std::string &path) {
    if (path.empty() || path == "/") return false;
    crc_forget_all();
    return remove_recursive(host(v, path));
}

bool vol_mkdir(Vol v, const std::string &path) {
    const std::string h = host(v, path);
    struct stat st;
    if (::stat(h.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(h.c_str(), 0755) == 0;
}

bool vol_rename(Vol v, const std::string &from, const std::string &to) {
    crc_forget_all();
    struct stat st;
    if (::stat(host(v, to).c_str(), &st) == 0) return false;
    return ::rename(host(v, from).c_str(), host(v, to).c_str()) == 0;
}

bool vol_free_bytes(Vol v, uint64_t &out) {
    (void)v;
    struct statvfs sv;
    if (::statvfs(root().c_str(), &sv) != 0) return false;
    out = static_cast<uint64_t>(sv.f_bavail) * static_cast<uint64_t>(sv.f_frsize);
    return true;
}

// --------------------------------------------------------------------------
// VolFile (emulator): both volumes stay open.
// --------------------------------------------------------------------------

struct VolFile::Impl {
    Vol v = Vol::Int;
    std::string path;
    FILE *f = nullptr;
    uint32_t pos = 0;

    void close_file() {
        if (f) std::fclose(f);
        f = nullptr;
    }
};

VolFile::VolFile() : impl_(new Impl) {}

VolFile::~VolFile() { close(); }

void VolFile::close() { impl_->close_file(); }

bool VolFile::open_read(Vol v, const std::string &path) {
    close();
    impl_->v = v;
    impl_->path = path;
    const std::string h = host(v, path);
    struct stat st;
    if (::stat(h.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return false;
    impl_->f = std::fopen(h.c_str(), "rb");
    impl_->pos = 0;
    return impl_->f != nullptr;
}

bool VolFile::open_write(Vol v, const std::string &path) {
    close();
    impl_->v = v;
    impl_->path = path;
    crc_forget_path(v, path);
    impl_->f = std::fopen(host(v, path).c_str(), "wb"); /* create + truncate */
    impl_->pos = 0;
    return impl_->f != nullptr;
}

bool VolFile::read_at(uint32_t offset, uint8_t *buf, size_t n) {
    Impl &m = *impl_;
    if (!m.f) {
        const std::string h = host(m.v, m.path);
        struct stat st;
        if (::stat(h.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return false;
        m.f = std::fopen(h.c_str(), "rb");
        if (!m.f) return false;
        m.pos = 0;
    }
    bool ok = true;
    if (m.pos != offset) {
        ok = std::fseek(m.f, static_cast<long>(offset), SEEK_SET) == 0;
        if (ok) m.pos = offset;
    }
    size_t got = 0;
    while (ok && got < n) {
        const size_t r = std::fread(buf + got, 1, n - got, m.f);
        if (r == 0) break;
        got += r;
    }
    m.pos += static_cast<uint32_t>(got);
    ok = ok && got == n;
    if (!ok) m.close_file();
    return ok;
}

bool VolFile::write_at(uint32_t offset, const uint8_t *buf, size_t n) {
    Impl &m = *impl_;
    crc_forget_path(m.v, m.path);
    if (!m.f) {
        const std::string h = host(m.v, m.path);
        struct stat st;
        if (::stat(h.c_str(), &st) != 0) return false;
        m.f = std::fopen(h.c_str(), "ab");
        if (!m.f) return false;
        m.pos = static_cast<uint32_t>(st.st_size);
    }
    if (m.pos != offset) {
        m.close_file();
        return false;
    }
    const size_t wrote = n ? std::fwrite(buf, 1, n, m.f) : 0;
    m.pos += static_cast<uint32_t>(wrote);
    if (wrote != n) {
        m.close_file();
        return false;
    }
    return true;
}

#endif  // ARDUINO

// --------------------------------------------------------------------------
// Whole-file CRC cache (both builds)
// --------------------------------------------------------------------------

namespace {
/* Notes manifests are the big user (internal keeps <= 50 notes before the
 * prune sweep); 64 entries of ~60 B + path. */
constexpr size_t CRC_CACHE_ENTRIES = 64;
std::mutex g_crc_mu;
CrcCache g_crc_cache(CRC_CACHE_ENTRIES);
uint32_t g_crc_forgets = 0; /* bumped by every invalidation, under g_crc_mu */

void crc_forget_path(Vol v, const std::string &path) {
    std::lock_guard<std::mutex> g(g_crc_mu);
    g_crc_cache.evict(static_cast<uint8_t>(v), path);
    ++g_crc_forgets;
}

/* A remove or rename may take a whole tree with it: forget everything. */
void crc_forget_all() {
    std::lock_guard<std::mutex> g(g_crc_mu);
    g_crc_cache.clear();
    ++g_crc_forgets;
}
}  // namespace

bool vol_file_crc(Vol v, const std::string &path, uint32_t size, uint32_t mtime, uint32_t &out) {
    const uint32_t gen = hw_get_filesystem_generation();
    const CrcKey key{static_cast<uint8_t>(v), path, size, mtime, gen};
    uint32_t forgets = 0;
    {
        std::lock_guard<std::mutex> g(g_crc_mu);
        if (g_crc_cache.lookup(key, out)) return true;
        forgets = g_crc_forgets;
    }

    VolFile f;
    if (!f.open_read(v, path)) return false;
    std::vector<uint8_t> buf(MAX_DATA_CHUNK); /* heap: worker stacks are small */
    uint32_t crc = 0;
    uint32_t off = 0;
    while (off < size) {
        const size_t n = std::min<size_t>(MAX_DATA_CHUNK, size - off);
        if (!f.read_at(off, buf.data(), n)) return false;
        crc = crc32_update(crc, buf.data(), n);
        off += static_cast<uint32_t>(n);
    }
    f.close();
    out = crc;

    /* A background note save bumps the generation before its bytes land, so
     * a pass that overlaps one may have read the old bytes; a write through
     * this layer during the pass bumps g_crc_forgets. Either way, don't keep it. */
    if (!hw_save_pending() && hw_get_filesystem_generation() == gen) {
        std::lock_guard<std::mutex> g(g_crc_mu);
        if (g_crc_forgets == forgets) g_crc_cache.store(key, crc);
    }
    return true;
}

void vol_crc_forget(Vol v, const std::string &path) { crc_forget_path(v, path); }

}  // namespace pagerlink
