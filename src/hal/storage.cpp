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
#include "../core/input_profile.h"

#include <atomic>
#include <cstring>

#ifdef ARDUINO
#include <algorithm>
#include <set>
#include <LilyGoLib.h>
#include <SD.h>
#include <FFat.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
/* Cheap on every save — FFat.mkdir is a no-op when the entry exists. The SD
 * branch only fires when the card is mounted; without a card the FFat copy
 * is the user's only home. */
static void ensure_notes_dir()
{
    if (!FFat.exists(NOTES_DIR)) FFat.mkdir(NOTES_DIR);
    if (HW_SD_ONLINE & hw_get_device_online()) {
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

void hw_get_storage_info(uint64_t &total, uint64_t &used, uint64_t &free)
{
    total = 0;
    used = 0;
    free = 0;
#if defined(ARDUINO)
#if defined(HAS_SD_CARD_SOCKET)
    if (HW_SD_ONLINE & hw_get_device_online()) {
        core::ScopedSpiLock lock;
        total = SD.totalBytes();
        used = SD.usedBytes();
        free = total - used;
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

void hw_mount_sd()
{
#if defined(ARDUINO) && defined(HAS_SD_CARD_SOCKET)
    instance.installSD();
#endif
}

static bool filesystem_dirty = false;

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

// Persisted across boots so consumers (e.g. the journal index) can trust a
// cached snapshot when nothing has been written since the last refresh. The
// flag lives in NVS namespace "fs"/"dirty" — lazy-loaded on first access and
// only written when the value actually changes, so per-save callers pay the
// flash cost at most once per dirty/clean transition.
static bool dirty_loaded = false;

static void load_filesystem_dirty()
{
#ifdef ARDUINO
    if (dirty_loaded) return;
    Preferences p;
    if (p.begin("fs", true)) {
        filesystem_dirty = p.getBool("dirty", false);
        p.end();
    }
    dirty_loaded = true;
#else
    dirty_loaded = true;
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

uint32_t hw_get_filesystem_generation()
{
    return filesystem_generation.load();
}

void hw_set_filesystem_dirty(bool dirty)
{
    if (dirty) filesystem_generation++;
    load_filesystem_dirty();
    if (filesystem_dirty == dirty) return;
    filesystem_dirty = dirty;
#ifdef ARDUINO
    Preferences p;
    if (p.begin("fs", false)) {
        p.putBool("dirty", dirty);
        p.end();
    }
#endif
}

bool hw_delete_file(const char *path)
{
    hw_set_filesystem_dirty(true);
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));

    bool res_sd = false;
    bool sd_online = (HW_SD_ONLINE & hw_get_device_online());
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
    std::string str(buf); // delete_path_recursive uses std::string children list
    if (use_sd) {
        if (!(HW_SD_ONLINE & hw_get_device_online())) return false;
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

bool hw_read_file(const char *path, std::string &content)
{
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));
    File f;
    core::MaybeSpiLock lock;
    bool is_sd = (HW_SD_ONLINE & hw_get_device_online());

    if (is_sd) {
        lock.acquire();
        f = SD.open(str, FILE_READ);
        if (!f) {
            lock.release();
        }
    }

    if (!f) {
        f = FFat.open(str, FILE_READ);
    }

    if (!f) {
        log_e("Failed to open file for reading: %s", str);
        return false;
    }

    hal::fs_read_all(f, content);
    log_d("Read %u bytes from %s (%s)", (unsigned int)content.size(), str, lock.held() ? "SD" : "Internal");
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
    File f;
    core::MaybeSpiLock lock;
    bool is_sd = (HW_SD_ONLINE & hw_get_device_online());
    size_t size = 0;

    if (is_sd) {
        lock.acquire();
        f = SD.open(str, FILE_READ);
        if (!f) lock.release();
    }

    if (!f) {
        f = FFat.open(str, FILE_READ);
    }

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
    if (!(HW_SD_ONLINE & hw_get_device_online())) return false;
    char str[256];
    normalize_path(path, str, sizeof(str));
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
    if (!(HW_SD_ONLINE & hw_get_device_online())) return false;
    char str[256];
    normalize_path(path, str, sizeof(str));

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
struct FileInfo {
    std::string name;
    time_t time;
};

static void list_files(std::vector<FileInfo> &list, fs::FS &fs, const char *dirname, const char *ext,
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
                // Skip file.getLastWrite() as it can trigger a slow f_stat lookup on some ESP32 FAT
                // implementations. We rely on the chronological filename fallback for sorting.
                list.push_back({filename.c_str(), 0});
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
    std::vector<FileInfo> file_infos;
    list_files(file_infos, FFat, NOTES_DIR, ".txt");

    std::sort(file_infos.begin(), file_infos.end(), [](const FileInfo& a, const FileInfo& b) {
        if (a.time != b.time) {
            return a.time > b.time;
        }
        return a.name > b.name;
    });

    for (const auto& fi : file_infos) {
        list.push_back(NOTES_PREFIX + fi.name);
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
    if (HW_SD_ONLINE & hw_get_device_online()) {
        std::vector<FileInfo> file_infos;
        {
            core::ScopedSpiLock lock;
            list_files(file_infos, SD, NOTES_DIR, ".txt");
        }

        std::sort(file_infos.begin(), file_infos.end(), [](const FileInfo & a, const FileInfo & b) {
            if (a.time != b.time) return a.time > b.time;
            return a.name > b.name;
        });

        for (const auto &fi : file_infos) {
            list.push_back(NOTES_PREFIX + fi.name);
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
    if (HW_SD_ONLINE & hw_get_device_online()) {
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
    std::vector<FileInfo> infos;
    list_files(infos, FFat, NOTES_DIR, ".txt");

    if (infos.size() < kEvictThreshold) {
        if (cb) cb(0, 0, "No eviction needed.");
        return;
    }

    // SD is required — without it we'd be deleting the only copy.
    bool sd_online = (HW_SD_ONLINE & hw_get_device_online());
    if (!sd_online) {
        hw_mount_sd();
        sd_online = (HW_SD_ONLINE & hw_get_device_online());
    }
    if (!sd_online) {
        if (cb) cb(0, 0, "SD unavailable; eviction skipped.");
        return;
    }
    ensure_notes_dir();

    // Sort by time ascending (oldest first) so the batch we move out is the
    // least-recently-touched notes.
    std::sort(infos.begin(), infos.end(), [](const FileInfo &a, const FileInfo &b) {
        if (a.time != b.time) return a.time < b.time;
        return a.name < b.name;
    });

    size_t total_to_move = std::min<size_t>(kEvictBatch, infos.size());

    for (size_t i = 0; i < total_to_move; ++i) {
        // list_files returns leaf names; resolve back to absolute paths under
        // /notes for both the read (FFat) and write (SD) sides.
        String leaf = infos[i].name.c_str();
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

bool hw_save_preferred_file(const char *path, const char *content, std::string *error,
                            bool allow_prune)
{
    hw_set_filesystem_dirty(true);

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

    if (HW_SD_ONLINE & hw_get_device_online()) {
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

bool hw_read_preferred_file_snippet(const char *path, std::string &content, size_t max_bytes, bool *truncated)
{
    content.clear();
    if (truncated) *truncated = false;
#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));

    File f = FFat.open(str, FILE_READ);
    core::MaybeSpiLock lock;
    if (!f) {
        if (HW_SD_ONLINE & hw_get_device_online()) {
            lock.acquire();
            f = SD.open(str, FILE_READ);
        }
    }

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
    std::vector<FileInfo> infos;

    // Scan Internal
    list_files(infos, FFat, NOTES_DIR, ".txt", cb);

    // Scan SD if available
    if (HW_SD_ONLINE & hw_get_device_online()) {
        std::vector<FileInfo> sd_infos;
        {
            core::ScopedSpiLock lock;
            list_files(sd_infos, SD, NOTES_DIR, ".txt", cb);
        }

        // Merge SD into infos, avoiding duplicates (Internal wins). Set-backed
        // lookup — this runs on every journal reconcile, so the old O(n²) scan
        // hurt most here.
        std::set<std::string> internal_names;
        for (const auto &ii : infos) internal_names.insert(ii.name);
        for (const auto &sdi : sd_infos) {
            if (!internal_names.count(sdi.name)) infos.push_back(sdi);
        }
    }

    list.reserve(infos.size());
    for (const auto &fi : infos) {
        list.emplace_back(NOTES_PREFIX + fi.name, (uint32_t)fi.time);
    }
#endif
}

bool hw_save_preferred_bytes(const char *path, const uint8_t *buf, size_t len, std::string *error)
{
    // Don't set dirty for the index file itself!
    bool is_index = (strstr(path, "_idx.bin") != nullptr);
    if (!is_index) {
        hw_set_filesystem_dirty(true);
    }

#ifdef ARDUINO
    char str[256];
    normalize_path(path, str, sizeof(str));

    // Save to Internal. A failure still falls through to the SD copy and the
    // prune check (which may free the space that made it fail).
    bool ok = hal::fs_write_all(FFat, str, buf, len);
    if (!ok && error) *error = "Cannot write to Internal.";

    // Redundant save to SD if available
    if (HW_SD_ONLINE & hw_get_device_online()) {
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
    File f;
    core::MaybeSpiLock lock;

    // Try Internal first (faster)
    f = FFat.open(str, FILE_READ);

    // Fallback to SD if missing from Internal
    if (!f && (HW_SD_ONLINE & hw_get_device_online())) {
        lock.acquire();
        f = SD.open(str, FILE_READ);
        if (!f) lock.release();
    }

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
