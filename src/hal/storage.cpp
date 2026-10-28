/**
 * @file      storage.cpp
 * @brief     SD card and FFat filesystem operations.
 */
#include "storage.h"
#include "system.h"
#include "internal.h"
#include "notes_crypto.h"
#include "notes_path.h"
#include "fs_util.h"
#include "evict.h"
#include "../core/spi_lock.h"
#include "../core/kb_lock.h"
#include "../core/input_profile.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#ifdef ARDUINO
#include <algorithm>
#include <functional>
#include <set>
#include <LilyGoLib.h>
#include <SD.h>
#include <FFat.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#endif

/* When notes_crypto is enabled+unlocked, the text I/O wrappers below
 * transparently encrypt on save and decrypt on load for any path that
 * `notes_crypto_path_is_protected()` matches. On a locked session, a read
 * that hits encrypted content fails so callers don't show ciphertext. */

/* Notes app .txt files live under hal::NOTES_DIR on both internal FFat and
 * the SD card root (see notes_path.h). */
using hal::NOTES_DIR;
using hal::NOTES_PREFIX;

#ifdef ARDUINO
// IS.2 background save. Defined below hw_save_preferred_file. P7.19: callers
// wait only for a save that can affect them; paths are normalize_path()ed.
static void wait_pending_save();                        // any save (writers, note listings)
static void wait_pending_save_for(const char *path);    // a save of exactly `path`
static void wait_pending_save_under(const char *path);  // ... of `path` or anything under it
static void wait_pending_save_in_dir(const char *dir);  // ... of a direct child of `dir`
#else
static inline void wait_pending_save() {}
static inline void wait_pending_save_for(const char *) {}
static inline void wait_pending_save_under(const char *) {}
static inline void wait_pending_save_in_dir(const char *) {}
#endif

#ifdef ARDUINO
/* Cheap on every save — FFat.mkdir is a no-op when the entry exists. The SD
 * branch only fires when the card is mounted; without a card the FFat copy
 * is the user's only home. */
static void ensure_notes_dir()
{
    if (!FFat.exists(NOTES_DIR)) FFat.mkdir(NOTES_DIR);
    if (hw_sd_online()) {
        core::ScopedSpiLock lock;
        if (!SD.exists(NOTES_DIR)) SD.mkdir(NOTES_DIR);
    }
}

/* Returns true if the encoded bytes were written into `out`. Either a
 * passthrough (plaintext copy) or the ciphertext. Returns false only if the
 * caller should abort the save — i.e. crypto is enabled for this path but the
 * session is locked, which would otherwise silently leak plaintext to disk. */
static bool encode_for_write(const char *path, const char *content,
                              std::vector<uint8_t> &out, std::string *error)
{
    size_t n = content ? strlen(content) : 0;
    bool protect = notes_crypto_path_is_protected(path);
    bool enabled = notes_crypto_is_enabled();

    if (protect && enabled) {
        if (!notes_crypto_is_unlocked()) {
            if (error) *error = "Notes are locked. Unlock first.";
            return false;
        }
        if (!notes_crypto_encrypt_buffer((const uint8_t *)content, n, out)) {
            if (error) *error = "Encryption failed.";
            return false;
        }
        return true;
    }
    out.assign((const uint8_t *)content, (const uint8_t *)content + n);
    return true;
}

/* If the just-read string begins with the OpenSSL magic, decrypt it in place.
 * Returns false only if the magic was present but decryption failed; callers
 * treat that as a read error so the user never sees ciphertext as text. */
static bool decode_after_read(std::string &content)
{
    if (!content_has_salted_magic(content.data(), content.size())) return true;
    if (!notes_crypto_is_unlocked()) return false;
    std::string plain;
    if (!notes_crypto_decrypt_buffer((const uint8_t *)content.data(),
                                     content.size(), plain)) return false;
    content = std::move(plain);
    return true;
}
#endif

#if defined(ARDUINO) && defined(HAS_SD_CARD_SOCKET)
/* P7.28: SD usage cache. SD.totalBytes() and SD.usedBytes() each run
 * f_getfree() under the SPI lock; FatFs scans the whole FAT the first time per
 * mount on FAT12/16 or when FAT32's FSINFO count is invalid (FF_FS_NOFSINFO 0
 * trusts a valid one), then keeps free_clst in RAM. sys.info (link_task) and
 * Settings > Info poll this, so the numbers are cached and re-read only when
 * the filesystem generation moves (any write through storage.cpp or the link
 * fs handlers) or the card may have changed (hw_mount_sd, the light-sleep
 * remount: hw_storage_info_invalidate). Writes over USB MSC don't bump the
 * generation, so the figure can lag those until the next local write. */
static portMUX_TYPE s_sd_info_mux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<uint32_t> s_sd_info_epoch{1};
static uint32_t s_sd_info_cached_epoch = 0;   // 0 = nothing cached
static uint32_t s_sd_info_gen = 0;
static uint64_t s_sd_info_total = 0;
static uint64_t s_sd_info_used = 0;
#endif

#if defined(ARDUINO) && defined(HAS_SD_CARD_SOCKET)
// The SD card may have changed underneath (mount, sleep unmount): make the
// next hw_get_storage_info() re-read it.
static void hw_storage_info_invalidate()
{
    s_sd_info_epoch++;
}
#endif

void hw_get_storage_info(uint64_t &total, uint64_t &used, uint64_t &free)
{
    total = 0;
    used = 0;
    free = 0;
#if defined(ARDUINO)
#if defined(HAS_SD_CARD_SOCKET)
    if (hw_sd_online()) {
        // Sample both keys before the read, so a write or remount that lands
        // during it leaves the cache keyed to the older state (re-read next time).
        const uint32_t epoch = s_sd_info_epoch.load();
        const uint32_t gen = hw_get_filesystem_generation();
        bool hit;
        portENTER_CRITICAL(&s_sd_info_mux);
        hit = s_sd_info_cached_epoch == epoch && s_sd_info_gen == gen;
        total = s_sd_info_total;
        used = s_sd_info_used;
        portEXIT_CRITICAL(&s_sd_info_mux);
        if (!hit) {
            {
                core::ScopedSpiLock lock;
                total = SD.totalBytes();
                used = SD.usedBytes();
            }
            portENTER_CRITICAL(&s_sd_info_mux);
            s_sd_info_total = total;
            s_sd_info_used = used;
            s_sd_info_gen = gen;
            s_sd_info_cached_epoch = total ? epoch : 0;  // f_getfree failed: retry
            portEXIT_CRITICAL(&s_sd_info_mux);
        }
        free = total > used ? total - used : 0;
    }
#elif defined(USING_FATFS)
    total = FFat.totalBytes();
    used = FFat.usedBytes();
    free = FFat.freeBytes();
#endif
#endif
}

void hw_get_local_storage_info(uint64_t &total, uint64_t &used, uint64_t &free)
{
    total = 0;
    used = 0;
    free = 0;
#if defined(ARDUINO)
#if defined(USING_FATFS)
    total = FFat.totalBytes();
    used = FFat.usedBytes();
    free = FFat.freeBytes();
#endif
#endif
}

/* P7.26: mount (or re-check) the SD card. The vendor installSD() runs
 * SD.begin() on the shared SPI bus and reads SD_DET on the XL9555 over I2C,
 * so it takes both locks in the documented order (kb_lock.h: instance -> SPI
 * -> I2C; callers hold neither, and the SPI mutex is not recursive). The I2C
 * lock spans SD.begin() too, because the SD_DET read sits inside the vendor
 * call; with no card that returns after the one read. installSD() itself now
 * sets or clears HW_SD_ONLINE ([LOCAL PATCH P7.26]), so callers that re-check
 * hw_get_device_online() after this see a card inserted after boot. */
void hw_mount_sd()
{
#if defined(ARDUINO) && defined(HAS_SD_CARD_SOCKET)
    {
        core::ScopedSpiLock spi;
        core::ScopedI2cLock i2c;
        instance.installSD();
    }
    hw_storage_info_invalidate();
#endif
}

bool hw_sd_online()
{
    return (HW_SD_ONLINE & hw_get_device_online()) != 0;
}

static std::atomic<void (*)()> s_sd_unmount_hook{nullptr};

void hw_set_sd_unmount_hook(void (*hook)())
{
    s_sd_unmount_hook.store(hook);
}

/* P7.34: the sleep paths' half of the vendor's old SD unmount/remount, done
 * here so the SPI lock is taken first (kb_lock.h order) instead of inside
 * lightSleep() under the caller's I2C lock. The hook closes every kept-open
 * SD file while the card is still mounted. */
void hw_sd_unmount_for_sleep()
{
#if defined(ARDUINO) && defined(HAS_SD_CARD_SOCKET)
    {
        core::ScopedSpiLock spi;
        if (void (*hook)() = s_sd_unmount_hook.load()) hook();
        SD.end();
    }
    hw_storage_info_invalidate();
#endif
}

static std::atomic<bool> filesystem_dirty{false};  // P7.27: written from several tasks

// Vendor hooks from lib/LilyGoLib/src/USB_MSC.cpp — no header is exposed
// upstream, so we declare them here rather than reaching into the lib/.
extern bool is_usb_msc_reading();
extern bool is_usb_msc_writing();
extern bool is_usb_msc_mounted();

// MSC state only exists on hardware builds using the TinyUSB (OTG) stack,
// i.e. ARDUINO_USB_MODE=0 (what env_arduino sets); otherwise there is no
// USB mass-storage device and nothing is ever mounted.
#if defined(ARDUINO) && !ARDUINO_USB_MODE
bool hw_is_usb_msc_reading() { return is_usb_msc_reading(); }
bool hw_is_usb_msc_writing() { return is_usb_msc_writing(); }
bool hw_is_usb_msc_mounted() { return is_usb_msc_mounted(); }
#else
bool hw_is_usb_msc_reading() { return false; }
bool hw_is_usb_msc_writing() { return false; }
bool hw_is_usb_msc_mounted() { return false; }
#endif

// P7.27: the dirty flag lives in RAM (`filesystem_dirty`); NVS "fs"/"dirty"
// only has to answer "may the on-disk journal index be stale?" on the next
// boot, which the journal uses to trust the index without a rescan.
//  - clean -> dirty: written (true) synchronously, before the caller's file
//    write, but only when NVS still says clean. A crash or power loss after an
//    edit therefore always boots dirty, as before.
//  - dirty -> clean (journal reconciled): RAM only. NVS keeps "dirty" until
//    hw_filesystem_dirty_persist() writes false at sleep or shutdown, and only
//    if RAM is still clean then. A crash in between costs one extra rescan.
// So an edit -> journal cycle costs no NVS write once NVS says dirty, instead
// of two (true, then false), and at most one true plus one false per session.
static bool dirty_loaded = false;
static std::atomic<bool> nvs_dirty{false};   // what NVS holds (after the load)

static void load_filesystem_dirty()
{
#ifdef ARDUINO
    if (dirty_loaded) return;
    Preferences p;
    if (p.begin("fs", true)) {
        filesystem_dirty = p.getBool("dirty", false);
        p.end();
    }
    nvs_dirty = filesystem_dirty.load();
    dirty_loaded = true;
#else
    dirty_loaded = true;
#endif
}

#ifdef ARDUINO
// Serializes the NVS writes below. The flags are seq_cst atomics: a writer
// stores filesystem_dirty then loads nvs_dirty; the persist stores nvs_dirty
// then re-loads filesystem_dirty, so at least one of them sees the other and
// NVS never ends up "clean" behind a write that started before it.
static std::mutex s_nvs_dirty_mutex;

static void write_nvs_dirty(bool dirty)
{
    Preferences p;
    if (p.begin("fs", false)) {
        p.putBool("dirty", dirty);
        p.end();
    }
}
#endif

void hw_filesystem_dirty_persist()
{
#ifdef ARDUINO
    load_filesystem_dirty();
    std::lock_guard<std::mutex> g(s_nvs_dirty_mutex);
    if (filesystem_dirty || !nvs_dirty.load()) return;
    nvs_dirty = false;
    write_nvs_dirty(false);
    if (filesystem_dirty) {           // a write raced the clean: put it back
        write_nvs_dirty(true);
        nvs_dirty = true;
    }
#endif
}

bool hw_get_filesystem_dirty()
{
    load_filesystem_dirty();
    return filesystem_dirty;
}

// Bumped on every hw_set_filesystem_dirty(true), even when the flag is already
// set: the flag is cleared by whichever app reloads first, so observers that
// must not steal it (the PagerLink evt.fs.changed) watch this counter instead.
static std::atomic<uint32_t> filesystem_generation{0};

static std::atomic<void (*)()> filesystem_generation_hook{nullptr};

uint32_t hw_get_filesystem_generation()
{
    return filesystem_generation.load();
}

void hw_set_filesystem_generation_hook(void (*hook)())
{
    filesystem_generation_hook.store(hook);
}

void hw_set_filesystem_dirty(bool dirty)
{
    if (dirty) {
        filesystem_generation++;
        if (void (*hook)() = filesystem_generation_hook.load()) hook();
    }
    load_filesystem_dirty();
    filesystem_dirty = dirty;
#ifdef ARDUINO
    // P7.27: only the first clean -> dirty since NVS last said clean is
    // written, and before the caller's file write (later callers wait for it).
    if (dirty && !nvs_dirty.load()) {
        std::lock_guard<std::mutex> g(s_nvs_dirty_mutex);
        if (!nvs_dirty.load()) {
            write_nvs_dirty(true);
            nvs_dirty = true;
        }
    }
#endif
}

/* Per-note write stamps (RAM only). The listing cannot carry an mtime
 * (DC2.3.18), so the journal's index would reuse a note's cached preview
 * after the note was rewritten under the same name (its own editor, the
 * phone over PagerLink). Every write or delete of /notes/<leaf>.txt takes a
 * fresh stamp from a counter seeded at random per boot; the listing reports
 * it in the mtime slot, so a changed stamp means "re-read the preview". A
 * note untouched this boot reports 0: an index entry stamped in an earlier
 * boot is re-read once and then settles back on 0. */
namespace {
std::mutex s_note_stamp_mutex;
std::map<std::string, uint32_t> s_note_stamps;  // leaf name -> stamp
uint32_t s_note_stamp_next = 0;

/* The leaf of a "/notes/<leaf>" or "notes/<leaf>" path, or nullptr. */
const char *note_leaf(const char *path)
{
    if (!path) return nullptr;
    while (*path == '/') path++;
    const size_t plen = strlen(hal::NOTES_PREFIX);
    if (strncmp(path, hal::NOTES_PREFIX, plen) != 0) return nullptr;
    const char *leaf = path + plen;
    if (!*leaf || strchr(leaf, '/')) return nullptr;
    return leaf;
}

uint32_t note_stamp(const std::string &leaf)
{
    std::lock_guard<std::mutex> g(s_note_stamp_mutex);
    auto it = s_note_stamps.find(leaf);
    return it == s_note_stamps.end() ? 0u : it->second;
}
}  // namespace

void hw_note_touched(const char *path)
{
    const char *leaf = note_leaf(path);
    if (!leaf) return;
    std::lock_guard<std::mutex> g(s_note_stamp_mutex);
    if (s_note_stamp_next == 0) {
#ifdef ARDUINO
        s_note_stamp_next = esp_random();
#else
        s_note_stamp_next = 1;
#endif
    }
    if (++s_note_stamp_next == 0) ++s_note_stamp_next;  // 0 means "untouched"
    s_note_stamps[leaf] = s_note_stamp_next;
}

bool hw_delete_file(const char *path)
{
    hw_set_filesystem_dirty(true);
    hw_note_touched(path);
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);

    bool res_sd = false;
    bool sd_online = hw_sd_online();
    if (sd_online) {
        core::ScopedSpiLock lock;
        res_sd = SD.remove(str);
    }
    bool res_int = FFat.remove(str);
    return res_sd || res_int;
#else
    log_d("Delete file: %s", path);
    return true;
#endif
}

#ifdef ARDUINO
/* Recursively walk and remove. Children are snapshotted first because
 * openNextFile() iterators don't survive mutations. */
static bool delete_path_recursive(fs::FS &fs, const std::string &path)
{
    File f = fs.open(path.c_str());
    if (!f) return false;
    if (!f.isDirectory()) {
        f.close();
        return fs.remove(path.c_str());
    }

    std::vector<std::string> children;
    File entry = f.openNextFile();
    while (entry) {
        const char* name = entry.name();
        const char* slash = strrchr(name, '/');
        if (slash) name = slash + 1;
        std::string full = path;
        if (!full.empty() && full.back() != '/') full += "/";
        full += name;
        children.push_back(full);
        entry.close();
        entry = f.openNextFile();
    }
    f.close();

    for (const auto &c : children) {
        if (!delete_path_recursive(fs, c)) return false;
    }
    return fs.rmdir(path.c_str());
}
#endif

bool hw_delete_path(const char *path, bool use_sd)
{
    hw_set_filesystem_dirty(true);
#ifdef ARDUINO
    if (!path || !path[0]) return false;
    char buf[256];
    normalize_path(path, buf, sizeof(buf));
    wait_pending_save_under(buf);
    std::string str(buf); // delete_path_recursive uses std::string children list
    if (use_sd) {
        if (!hw_sd_online()) return false;
        core::ScopedSpiLock lock;
        return delete_path_recursive(SD, str);
    }
    return delete_path_recursive(FFat, str);
#else
    (void)use_sd;
    log_d("Delete path: %s", path);
    return true;
#endif
}

#ifdef ARDUINO
/* DC2.4.22: the one SD-or-internal fallback behind the whole-file readers.
 * An SD hit leaves `lock` held (the caller drops it when done with the file);
 * an SD miss releases it before falling back. `sd_first` keeps each caller's
 * existing order: hw_read_file/hw_get_file_size prefer the SD copy, the
 * snippet/bytes readers the internal one. The two can disagree only when the
 * copies diverge (an edit over USB MSC on the SD card, a failed SD mirror
 * write); unifying the order is a product call (DC2.3.19), so it is kept. */
static File open_preferred(const char *path, core::MaybeSpiLock &lock, bool sd_first)
{
    auto open_sd = [&]() -> File {
        if (!hw_sd_online()) return File();
        lock.acquire();
        File f = SD.open(path, FILE_READ);
        if (!f) lock.release();
        return f;
    };
    File f = sd_first ? open_sd() : FFat.open(path, FILE_READ);
    if (!f) f = sd_first ? FFat.open(path, FILE_READ) : open_sd();
    return f;
}
#endif

bool hw_read_file(const char *path, std::string &content)
{
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);
    core::MaybeSpiLock lock;
    File f = open_preferred(str, lock, /*sd_first=*/true);

    if (!f) {
        log_e("Failed to open file for reading: %s", str);
        return false;
    }

    hal::fs_read_all(f, content);
    log_d("Read %u bytes from %s (%s)", (unsigned int)content.size(), str, lock.held() ? "SD" : "Internal");
    // Drop the bus before decrypting: a PBKDF2 miss is ~2 s, and holding the
    // SPI lock through it stalls display flushes and every other SD user (P7.24).
    f.close();
    lock.release();
    if (!decode_after_read(content)) {
        content.clear();
        return false;
    }
    return true;
#else
    log_d("Read from file: %s", path);
    content = "Dummy content for simulation";
    return true;
#endif
}

size_t hw_get_file_size(const char *path)
{
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);
    core::MaybeSpiLock lock;
    File f = open_preferred(str, lock, /*sd_first=*/true);
    size_t size = 0;

    if (f) {
        size = f.size();
        f.close();
    }
    return size;
#else
    return 1024;
#endif
}

bool hw_read_internal_bytes_raw(const char *path, std::vector<uint8_t> &buf)
{
    buf.clear();
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);
    return hal::fs_read_all(FFat, str, buf);
#else
    (void)path;
    return false;
#endif
}

bool hw_read_sd_bytes_raw(const char *path, std::vector<uint8_t> &buf)
{
    buf.clear();
#ifdef ARDUINO
    if (!hw_sd_online()) return false;
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);
    core::ScopedSpiLock lock;
    return hal::fs_read_all(SD, str, buf);
#else
    (void)path;
    return false;
#endif
}

bool hw_read_sd_stream(const char *path, size_t chunk_bytes,
                       bool (*sink)(void *user, const uint8_t *data, size_t len),
                       void *user)
{
#ifdef ARDUINO
    if (!sink || chunk_bytes == 0) return false;
    if (!hw_sd_online()) return false;
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);

    File f;
    { core::ScopedSpiLock lock; f = SD.open(str, FILE_READ); }
    if (!f) return false;

    std::vector<uint8_t> buf(chunk_bytes);
    bool ok = true;
    while (true) {
        // Fill a whole chunk under one lock, then drop the bus so an LVGL
        // flush (core 1) can interleave before the next chunk.
        size_t filled = 0;
        {
            core::ScopedSpiLock lock;
            while (filled < chunk_bytes) {
                int r = f.read(buf.data() + filled, chunk_bytes - filled);
                if (r <= 0) break;
                filled += (size_t)r;
            }
        }
        if (filled == 0) break;                 // EOF
        if (!sink(user, buf.data(), filled)) { ok = false; break; }
        if (filled < chunk_bytes) break;        // short read → EOF
    }
    { core::ScopedSpiLock lock; f.close(); }
    return ok;
#else
    (void)path; (void)chunk_bytes; (void)sink; (void)user;
    return false;
#endif
}

#ifdef ARDUINO
/* Leaf names of the `ext` files in `dirname`, unsorted. No modification time:
 * file.getLastWrite() can trigger a slow f_stat lookup on the ESP32 FAT, and
 * note filenames are chronological, so callers sort by name (DC2.3.18). */
static void list_files(std::vector<std::string> &list, fs::FS &fs, const char *dirname, const char *ext,
                       void (*cb)(int, int, const char *) = nullptr)
{
    File root = fs.open(dirname);
    if (!root || !root.isDirectory()) return;

    File file = root.openNextFile();
    int count = 0;
    while (file) {
        if (!file.isDirectory()) {
            String filename = file.name();
            // Normalize to leaf name - SD returns full path, FFat returns leaf.
            int slash = filename.lastIndexOf('/');
            if (slash >= 0) filename = filename.substring(slash + 1);

            if (filename.endsWith(ext)) {
                list.push_back(filename.c_str());
                count++;
                if (cb) cb(count, 0, filename.c_str());
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
}
#endif

void hw_get_internal_txt_files(std::vector<std::string> &list)
{
    list.clear();
#ifdef ARDUINO
    wait_pending_save();
    std::vector<std::string> names;
    list_files(names, FFat, NOTES_DIR, ".txt");
    std::sort(names.begin(), names.end(), std::greater<std::string>());  // newest first

    for (const auto &name : names) {
        list.push_back(NOTES_PREFIX + name);
    }
#else
    list.push_back("notes/internal1.txt");
    list.push_back("notes/internal2.txt");
#endif
}

void hw_get_sd_txt_files(std::vector<std::string> &list)
{
    list.clear();
#ifdef ARDUINO
    wait_pending_save();
    if (hw_sd_online()) {
        std::vector<std::string> names;
        {
            core::ScopedSpiLock lock;
            list_files(names, SD, NOTES_DIR, ".txt");
        }
        std::sort(names.begin(), names.end(), std::greater<std::string>());  // newest first

        for (const auto &name : names) {
            list.push_back(NOTES_PREFIX + name);
        }
    }
#else
    list.push_back("notes/sd1.txt");
    list.push_back("notes/sd2.txt");
#endif
}

#ifdef ARDUINO
static void list_entries(std::vector<HwDirEntry> &list, fs::FS &fs,
                         const char *dirname, const char *filter_ext)
{
    File root = fs.open(dirname);
    if (!root || !root.isDirectory()) return;

    bool has_filter = filter_ext && filter_ext[0] != '\0';
    std::vector<HwDirEntry> dirs;
    std::vector<HwDirEntry> files;

    File entry = root.openNextFile();
    while (entry) {
        const char* name = entry.name();
        // Normalize to leaf name — SD returns full path, FFat returns leaf.
        const char* slash = strrchr(name, '/');
        if (slash) name = slash + 1;
        if (entry.isDirectory()) {
            // Skip entry.getLastWrite() as it triggers a slow f_stat lookup on FAT.
            dirs.push_back({std::string(name), true, 0u, 0u});
        } else {
            std::string name_str(name);
            if (!has_filter || (name_str.length() >= strlen(filter_ext) && name_str.compare(name_str.length() - strlen(filter_ext), strlen(filter_ext), filter_ext) == 0)) {
                files.push_back({name_str, false, 0u, (uint32_t)entry.size()});
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    auto by_path = [](const HwDirEntry &a, const HwDirEntry &b) { return a.path < b.path; };
    std::sort(dirs.begin(), dirs.end(), by_path);
    std::sort(files.begin(), files.end(), by_path);

    list.insert(list.end(), dirs.begin(), dirs.end());
    list.insert(list.end(), files.begin(), files.end());
}
#endif

void hw_list_internal_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                              const char *dirname)
{
    list.clear();
    if (!dirname || !dirname[0]) dirname = "/";
#ifdef ARDUINO
    {
        char dir[256];
        normalize_path(dirname, dir, sizeof(dir));
        wait_pending_save_in_dir(dir);
    }
    list_entries(list, FFat, dirname, filter_ext);
#else
    bool all = !(filter_ext && filter_ext[0]);
    bool at_root = (strcmp(dirname, "/") == 0);
    if (at_root) {
        list.push_back({"notes",         true,  1710000000, 0u});
        list.push_back({"drafts",        true,  1711000000, 0u});
        list.push_back({"internal1.txt", false, 1712000000, 256u});
        list.push_back({"internal2.txt", false, 1713000000, 4096u});
        if (all) {
            list.push_back({"readme.md", false, 1714000000, 1536u});
            list.push_back({"data.bin",  false, 1715000000, 1572864u});
        }
    } else if (strcmp(dirname, NOTES_DIR) == 0) {
        list.push_back({"hello.txt",  false, 1712100000, 42u});
        list.push_back({"ideas.txt",  false, 1712200000, 812u});
    } else if (strcmp(dirname, "/drafts") == 0) {
        list.push_back({"wip.txt",    false, 1711100000, 128u});
    }
#endif
}

void hw_list_sd_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                        const char *dirname)
{
    list.clear();
    if (!dirname || !dirname[0]) dirname = "/";
#ifdef ARDUINO
    {
        char dir[256];
        normalize_path(dirname, dir, sizeof(dir));
        wait_pending_save_in_dir(dir);
    }
    if (hw_sd_online()) {
        core::ScopedSpiLock lock;
        list_entries(list, SD, dirname, filter_ext);
    }
#else
    bool all = !(filter_ext && filter_ext[0]);
    bool at_root = (strcmp(dirname, "/") == 0);
    if (at_root) {
        list.push_back({"md",      true,  1710500000, 0u});
        list.push_back({"photos",  true,  1711500000, 0u});
        list.push_back({"sd1.txt", false, 1712500000, 320u});
        list.push_back({"sd2.txt", false, 1713500000, 2048u});
        if (all) {
            list.push_back({"track.mp3", false, 1714500000, 4194304u});
            list.push_back({"image.jpg", false, 1715500000, 524288u});
        }
    } else if (strcmp(dirname, "/md") == 0) {
        list.push_back({"note1.md", false, 1710600000, 640u});
        list.push_back({"note2.md", false, 1710700000, 1280u});
    } else if (strcmp(dirname, "/photos") == 0) {
        list.push_back({"pic1.jpg", false, 1711600000, 262144u});
    }
#endif
}

uint32_t hw_count_internal_files()
{
#ifdef ARDUINO
    /* Status-bar indicator counts user notes (under /notes), not bookkeeping
     * files at the FFat root like tasks.txt or journal_idx.bin. */
    uint32_t count = 0;
    File dir = FFat.open(NOTES_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) count++;
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return count;
#else
    return 4;
#endif
}

void hw_set_msc_prefer_sd(bool prefer_sd)
{
    user_setting.msc_prefer_sd = prefer_sd ? 1 : 0;
#ifdef ARDUINO
    instance.setMSCPreferSD(prefer_sd);
    save_user_setting_nvs();
    ui_msg_pop_up("USB MSC", "Settings updated.\nPlease reboot to apply\nMSC target change.");
#endif
}

void hw_prune_internal_storage(void (*cb)(int, int, const char *))
{
#ifdef ARDUINO
    /* Batch-eviction policy: when the internal store hits the threshold, move
     * the kEvictBatch oldest notes to SD in one shot, leaving headroom so the
     * next eviction is dozens of saves away. This trades many tiny prunes for
     * one larger I/O burst — fewer chances for a half-finished move to leave
     * the user wondering where a note went. */
    constexpr size_t kEvictThreshold = 50;
    constexpr size_t kEvictBatch     = 35;

    if (cb) cb(0, 0, "Scanning internal storage...");
    std::vector<std::string> infos;
    list_files(infos, FFat, NOTES_DIR, ".txt");

    if (infos.size() < kEvictThreshold) {
        if (cb) cb(0, 0, "No eviction needed.");
        return;
    }

    // SD is required — without it we'd be deleting the only copy.
    bool sd_online = hw_sd_online();
    if (!sd_online) {
        hw_mount_sd();
        sd_online = hw_sd_online();
    }
    if (!sd_online) {
        if (cb) cb(0, 0, "SD unavailable; eviction skipped.");
        return;
    }
    ensure_notes_dir();

    // Oldest first (chronological filenames), so the batch we move out is
    // the oldest notes.
    std::sort(infos.begin(), infos.end());

    size_t total_to_move = std::min<size_t>(kEvictBatch, infos.size());

    for (size_t i = 0; i < total_to_move; ++i) {
        // list_files returns leaf names; resolve back to absolute paths under
        // /notes for both the read (FFat) and write (SD) sides.
        String leaf = infos[i].c_str();
        String path = String(NOTES_DIR) + "/" + leaf;

        if (cb) cb((int)i, (int)total_to_move, ("Moving to SD: " + path).c_str());

        hal::EvictResult r = hal::evict_one(
            [&](std::vector<uint8_t> &buf) {
                return hal::fs_read_all(FFat, path.c_str(), buf);
            },
            [&](const std::vector<uint8_t> &buf) {
                core::ScopedSpiLock lock;
                return hal::fs_write_all(SD, path.c_str(), buf.data(), buf.size());
            },
            [&]() { FFat.remove(path); });

        if (r == hal::EvictResult::CopyFailed) {
            log_e("Eviction copy failed, keeping: %s", path.c_str());
        } else if (r == hal::EvictResult::Moved) {
            log_d("Eviction move: %s", path.c_str());
        }
    }

    if (cb) cb((int)total_to_move, (int)total_to_move, "Eviction complete.");
    hw_set_filesystem_dirty(true);
#endif
}

#ifdef ARDUINO
/* An eviction sweep moves up to kEvictBatch notes to SD — seconds of blocking
 * flash + SD I/O. Running it inline on the save that triggered it meant a
 * note "save on exit" could stall the UI for
 * that whole burst. Detach it onto a one-shot task so the save (and the exit
 * that drove it) returns immediately. The flag serialises sweeps: at most one
 * runs at a time, and a save arriving mid-sweep just defers re-triggering to a
 * later save rather than stacking a second sweep.
 *
 * No app-level lock guards the FFat ops this task shares with the UI thread
 * (concurrent saves) and the notes-sync task: the ESP-IDF FATFS driver is built
 * with FF_FS_REENTRANT=1, so every FFat call is already serialised on a
 * per-volume mutex. A remove racing a scan is at worst a benign logical race
 * (a note being evicted may blink out of a listing mid-move) — never FATFS
 * corruption — and the note is preserved on SD either way. */
static volatile bool s_prune_task_running = false;

static void prune_task(void *arg)
{
    (void)arg;
    hw_prune_internal_storage(nullptr);
    s_prune_task_running = false;
    vTaskDelete(nullptr);
}

static void prune_internal_storage()
{
    if (!user_setting.prune_internal) return;

    // FFat directory scan is O(N) and slow, so only evaluate every 10 saves.
    // The eviction threshold is 50, so overshooting by 10 is perfectly safe.
    static int s_save_calls = 0;
    if (++s_save_calls < 10) return;
    s_save_calls = 0;

    // A sweep is already running in the background; let it finish and re-check
    // on a future save rather than piling on a concurrent one.
    if (s_prune_task_running) return;

    s_prune_task_running = true;
    // 8 KB matches the other background workers (notes-sync/weather); there's
    // no TLS stack to accommodate.
    if (xTaskCreate(prune_task, "notes_prune", 8192, nullptr, 2, nullptr) != pdPASS) {
        // Spawn failed (severe memory pressure) — fall back to a synchronous
        // sweep so eviction still happens.
        s_prune_task_running = false;
        hw_prune_internal_storage(nullptr);
    }
}
#endif

static bool save_preferred_file_impl(const char *path, const char *content, std::string *error,
                            bool allow_prune)
{
    hw_set_filesystem_dirty(true);
    hw_note_touched(path);

#ifdef ARDUINO
    /* Encode once so the internal and SD copies share identical bytes (and
     * therefore an identical salt when encrypted). */
    std::vector<uint8_t> payload;
    {
        INPUT_PROF_SPAN("save.encode");
        if (!encode_for_write(path, content, payload, error)) return false;
    }

    char str[256];
    normalize_path(path, str, sizeof(str));
    /* Notes app saves land under /notes/ on both filesystems; create the
     * directory lazily so the editor never has to. tasks.txt and the journal
     * index are at the root and don't need it. */
    const size_t dlen = strlen(NOTES_DIR);
    if (strncmp(str, NOTES_DIR, dlen) == 0 && str[dlen] == '/') {
        ensure_notes_dir();
    }
    bool ok_int;
    {
        INPUT_PROF_SPAN("save.ffat");
        ok_int = hal::fs_write_all(FFat, str, payload.data(), payload.size());
    }
    if (!ok_int && error) {
        *error = "Cannot write to Internal.";
    }

    if (hw_sd_online()) {
        INPUT_PROF_SPAN("save.sd");
        core::ScopedSpiLock lock;
        hal::fs_write_all(SD, str, payload.data(), payload.size());
    }

    if (allow_prune) prune_internal_storage();
    return ok_int;
#else
    (void)error;
    (void)allow_prune;
    log_d("Save to preferred file: %s, content: %s", path, content);
    return true;
#endif
}

bool hw_save_preferred_file(const char *path, const char *content, std::string *error,
                            bool allow_prune)
{
    wait_pending_save();   // keep write order behind a background save
    return save_preferred_file_impl(path, content, error, allow_prune);
}

#ifdef ARDUINO
namespace {
struct AsyncSave {
    std::string       path;
    std::string       content;
    hw_save_fail_cb_t on_fail;
};
// P7.19: s_save_pending and s_save_path change together under s_save_mux, so a
// reader never pairs the flag with another save's path. Waiters block on
// SAVE_IDLE_BIT (set while no save is in flight) instead of polling every 5 ms.
volatile bool      s_save_pending   = false;
char               s_save_path[256] = "";   // normalize_path() form
portMUX_TYPE       s_save_mux       = portMUX_INITIALIZER_UNLOCKED;
EventGroupHandle_t s_save_eg        = nullptr;   // created by the first async save
constexpr EventBits_t SAVE_IDLE_BIT = BIT0;
TaskHandle_t       s_save_task      = nullptr;

void save_set_idle()
{
    taskENTER_CRITICAL(&s_save_mux);
    s_save_pending = false;
    s_save_path[0] = '\0';
    taskEXIT_CRITICAL(&s_save_mux);
    if (s_save_eg) xEventGroupSetBits(s_save_eg, SAVE_IDLE_BIT);
}

enum class SaveMatch { Any, Path, Under, InDir };

// Length of `p` without trailing slashes ("/" stays 1).
size_t trimmed_len(const char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') --n;
    return n;
}

bool save_path_matches(const char *pending, const char *p, SaveMatch m)
{
    switch (m) {
    case SaveMatch::Any:
        return true;
    case SaveMatch::Path:
        return strcmp(pending, p) == 0;
    case SaveMatch::Under: {
        size_t n = trimmed_len(p);
        if (n == 1 && p[0] == '/') return true;
        return strncmp(pending, p, n) == 0 && (pending[n] == '\0' || pending[n] == '/');
    }
    case SaveMatch::InDir: {
        const char *slash = strrchr(pending, '/');
        if (!slash) return false;
        size_t parent = slash == pending ? 1 : (size_t)(slash - pending);  // parent of "/x" is "/"
        return trimmed_len(p) == parent && strncmp(pending, p, parent) == 0;
    }
    }
    return true;
}

void wait_save(const char *p, SaveMatch m)
{
    if (!s_save_eg) return;                                   // no async save has run yet
    if (xTaskGetCurrentTaskHandle() == s_save_task) return;   // the worker's own nested calls
    taskENTER_CRITICAL(&s_save_mux);
    bool hit = s_save_pending && (m == SaveMatch::Any || (p && save_path_matches(s_save_path, p, m)));
    taskEXIT_CRITICAL(&s_save_mux);
    if (hit) xEventGroupWaitBits(s_save_eg, SAVE_IDLE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

void save_task(void *arg)
{
    AsyncSave *job = static_cast<AsyncSave *>(arg);
    std::string err;
    // Off the UI thread, so the eviction sweep may run (it spawns its own task).
    bool ok = save_preferred_file_impl(job->path.c_str(), job->content.c_str(), &err, true);
    if (!ok && job->on_fail) job->on_fail(job->path.c_str(), err);
    delete job;
    s_save_task = nullptr;
    save_set_idle();
    vTaskDelete(nullptr);
}
} // namespace

// A save is tens of ms to a full PBKDF2 when the prewarm slot was consumed,
// so only wait for one that can affect the caller (P7.19). The worker's own
// nested calls never wait on itself.
static void wait_pending_save() { wait_save(nullptr, SaveMatch::Any); }
static void wait_pending_save_for(const char *path) { wait_save(path, SaveMatch::Path); }
static void wait_pending_save_under(const char *path) { wait_save(path, SaveMatch::Under); }
static void wait_pending_save_in_dir(const char *dir) { wait_save(dir, SaveMatch::InDir); }

bool hw_save_preferred_file_async(const char *path, std::string content,
                                  hw_save_fail_cb_t on_fail)
{
    wait_pending_save();
    hw_set_filesystem_dirty(true);   // caches see the change before the write lands
    AsyncSave *job = new AsyncSave{path, std::move(content), on_fail};
    if (!s_save_eg) {
        s_save_eg = xEventGroupCreate();
        if (s_save_eg) xEventGroupSetBits(s_save_eg, SAVE_IDLE_BIT);
    }
    if (s_save_eg) {
        char norm[256];
        normalize_path(path, norm, sizeof(norm));
        xEventGroupClearBits(s_save_eg, SAVE_IDLE_BIT);
        taskENTER_CRITICAL(&s_save_mux);
        s_save_pending = true;
        memcpy(s_save_path, norm, sizeof(s_save_path));
        taskEXIT_CRITICAL(&s_save_mux);
    }
    // 10 KB: AES-CBC + a possible PBKDF2 derive + FFat/SD writes; no TLS.
    // Without the event group nobody could wait for it, so save inline.
    if (!s_save_eg ||
        xTaskCreatePinnedToCore(save_task, "note_save", 10240, job, 2, &s_save_task, 0) != pdPASS) {
        s_save_task = nullptr;
        save_set_idle();
        std::string err;
        bool ok = save_preferred_file_impl(job->path.c_str(), job->content.c_str(), &err, false);
        if (!ok && on_fail) on_fail(job->path.c_str(), err);
        delete job;
        return ok;
    }
    return true;
}

void hw_save_flush() { wait_pending_save(); }
bool hw_save_pending() { return s_save_pending; }
#else
bool hw_save_preferred_file_async(const char *path, std::string content,
                                  hw_save_fail_cb_t on_fail)
{
    std::string err;
    bool ok = hw_save_preferred_file(path, content.c_str(), &err, false);
    if (!ok && on_fail) on_fail(path, err);
    return ok;
}
void hw_save_flush() {}
bool hw_save_pending() { return false; }
#endif

bool hw_read_preferred_file_snippet(const char *path, std::string &content, size_t max_bytes, bool *truncated)
{
    content.clear();
    if (truncated) *truncated = false;
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);
    core::MaybeSpiLock lock;
    File f = open_preferred(str, lock, /*sd_first=*/false);
    if (!f) return false;

    size_t total = f.size();

    /* Probe the first 8 bytes so encrypted files take the full-read path.
     * CBC ciphertext can't be partially decrypted, so for protected files we
     * must read the whole thing, decrypt, then truncate the plaintext. */
    uint8_t probe[8] = {0};
    size_t probe_len = total < 8 ? total : 8;
    if (probe_len) f.read(probe, probe_len);
    bool is_enc = content_has_salted_magic((const char *)probe, probe_len);

    if (is_enc) {
        /* Read the whole file and decrypt. */
        std::string full;
        full.resize(total);
        f.seek(0);
        if (total) f.read((uint8_t *)&full[0], total);
        f.close();
        lock.release();

        if (!decode_after_read(full)) return false;
        if (full.size() > max_bytes) {
            content.assign(full.data(), max_bytes);
            if (truncated) *truncated = true;
        } else {
            content = std::move(full);
        }
        return true;
    }

    /* Plaintext: cheap partial read like before. */
    f.seek(0);
    size_t to_read = total < max_bytes ? total : max_bytes;
    content.resize(to_read);
    if (to_read > 0) {
        f.read((uint8_t *)&content[0], to_read);
    }
    f.close();

    if (truncated) *truncated = total > max_bytes;
    return true;
#else
    (void)path; (void)max_bytes;
    content = "simulated preview...";
    if (truncated) *truncated = true;
    return true;
#endif
}

void hw_get_preferred_txt_files_info(std::vector<std::pair<std::string, uint32_t>> &list,
                                     void (*cb)(int, int, const char *))
{
    list.clear();
#ifdef ARDUINO
    wait_pending_save();
    std::vector<std::string> infos;

    // Scan Internal
    list_files(infos, FFat, NOTES_DIR, ".txt", cb);

    // Scan SD if available
    if (hw_sd_online()) {
        std::vector<std::string> sd_infos;
        {
            core::ScopedSpiLock lock;
            list_files(sd_infos, SD, NOTES_DIR, ".txt", cb);
        }

        // Merge SD into infos, avoiding duplicates (Internal wins). Set-backed
        // lookup — this runs on every journal reconcile, so the old O(n²) scan
        // hurt most here.
        std::set<std::string> internal_names(infos.begin(), infos.end());
        for (const auto &sdi : sd_infos) {
            if (!internal_names.count(sdi)) infos.push_back(sdi);
        }
    }

    list.reserve(infos.size());
    for (const auto &name : infos) {
        list.emplace_back(NOTES_PREFIX + name, note_stamp(name));  // stamp, not mtime
    }
#endif
}

bool hw_save_preferred_bytes(const char *path, const uint8_t *buf, size_t len, std::string *error)
{
    // Don't set dirty for the index file itself!
    bool is_index = (strstr(path, "_idx.bin") != nullptr);
    if (!is_index) {
        hw_set_filesystem_dirty(true);
        hw_note_touched(path);
    }

#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));

    // Save to Internal. A failure still falls through to the SD copy and the
    // prune check (which may free the space that made it fail).
    bool ok = hal::fs_write_all(FFat, str, buf, len);
    if (!ok && error) *error = "Cannot write to Internal.";

    // Redundant save to SD if available
    if (hw_sd_online()) {
        core::ScopedSpiLock lock;
        hal::fs_write_all(SD, str, buf, len);
    }

    // Only prune if it was a data file (not the index)
    if (!is_index) {
        prune_internal_storage();
    }

    return ok;
#else
    (void)path; (void)buf; (void)len; (void)error;
    return true;
#endif
}

bool hw_read_preferred_bytes(const char *path, std::vector<uint8_t> &buf)
{
    buf.clear();
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));
    wait_pending_save_for(str);  // DC2.3.19: like every other reader
    core::MaybeSpiLock lock;
    File f = open_preferred(str, lock, /*sd_first=*/false);  // internal is faster
    if (!f) return false;
    hal::fs_read_all(f, buf);
    return true;
#else
    (void)path;
    return false;
#endif
}

/* hw_copy_all_notes_to_hub and hw_copy_internal_to_sd live in
 * storage_bulk.cpp — extracted because they're coordination layers over
 * the public storage.h API and have no file-static dependencies here. */
