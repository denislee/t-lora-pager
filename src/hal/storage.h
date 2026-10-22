/**
 * @file      storage.h
 * @brief     SD card and FFat filesystem operations.
 */
#pragma once

#include "types.h"

void hw_mount_sd();
// SD total/used/free; cached and re-read only after a filesystem-generation
// bump or hw_storage_info_invalidate() (P7.28).
void hw_get_storage_info(uint64_t &total, uint64_t &used, uint64_t &free);
// The SD card may have changed underneath (the vendor light-sleep remount):
// make the next hw_get_storage_info() re-read it.
void hw_storage_info_invalidate();
void hw_get_local_storage_info(uint64_t &total, uint64_t &used, uint64_t &free);

// Delete `path` from both internal FFat and (when mounted) the SD card.
// True if either copy was removed.
bool hw_delete_file(const char *path);

// Delete a file or directory (recursive for directories) on the chosen
// filesystem. Returns true on success.
bool hw_delete_path(const char *path, bool use_sd);
bool hw_read_file(const char *path, std::string &content);
size_t hw_get_file_size(const char *path);

// Raw byte read from internal FFat, bypassing the notes-crypto decode path.
// Callers that want to ship the ciphertext as-is (e.g. the GitHub notes
// sync, which commits the opaque Salted__ blob and decrypts host-side)
// rely on this to avoid triggering an unlock or altering the bytes.
bool hw_read_internal_bytes_raw(const char *path, std::vector<uint8_t> &buf);

// Raw byte read from the SD card, same semantics as the internal variant.
// Returns false when the card is not mounted or the file is missing.
bool hw_read_sd_bytes_raw(const char *path, std::vector<uint8_t> &buf);

// Streams an SD file to `sink` in chunks of `chunk_bytes` (the final chunk may
// be shorter at EOF). The file is opened once but the SPI bus is released
// between chunks so the display can flush — use this instead of
// hw_read_sd_bytes_raw for large files that would otherwise hold the whole
// payload in RAM or freeze the UI. `sink` returning false aborts the read.
// When the sink base64-encodes, pass a multiple-of-3 `chunk_bytes` so chunk
// boundaries don't inject mid-stream padding. Returns false on open/read
// failure or if the sink aborts.
bool hw_read_sd_stream(const char *path, size_t chunk_bytes,
                       bool (*sink)(void *user, const uint8_t *data, size_t len),
                       void *user);
void hw_get_internal_txt_files(std::vector<std::string> &list);
void hw_get_sd_txt_files(std::vector<std::string> &list);

// Directory listing including folders. `filter_ext` (e.g. ".txt") limits files
// by extension; pass nullptr or "" to include all files. Folders are always
// included. `mtime` is a unix timestamp (0 when unknown).
struct HwDirEntry {
    std::string path;
    bool is_dir;
    uint32_t mtime;
    uint32_t size;  // bytes; 0 for directories
};
void hw_list_internal_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                              const char *dirname = "/");
void hw_list_sd_entries(std::vector<HwDirEntry> &list, const char *filter_ext,
                        const char *dirname = "/");

// Cheap non-allocating count of files (not directories) in the internal FFat
// root. Intended for the status-bar indicator — does not recurse.
uint32_t hw_count_internal_files();

// USB MSC preference: when true, the SD card is exposed over USB MSC.
// When false (default), the internal memory (FFat) is exposed.
void hw_set_msc_prefer_sd(bool prefer_sd);

bool hw_is_usb_msc_reading();
bool hw_is_usb_msc_writing();
bool hw_is_usb_msc_mounted();

// Filesystem dirty flag: set to true whenever a file is saved or deleted.
// Used by high-level apps to invalidate their caches.
bool hw_get_filesystem_dirty();
void hw_set_filesystem_dirty(bool dirty);
// P7.27: the flag is kept in RAM; NVS records only "dirty since the last clean
// persist" (written once, on the first clean -> dirty). Call before sleep or
// shutdown to write the lazy clean; a no-op unless RAM is clean and NVS dirty.
void hw_filesystem_dirty_persist();
// Incremented by every hw_set_filesystem_dirty(true). Lets an observer notice
// writes without clearing the flag the apps rely on.
uint32_t hw_get_filesystem_generation();
// One observer called right after every generation bump, on the bumping task
// (PagerLink wakes its link task so evt.fs.changed goes out without a poll,
// P7.35). It must only signal: no locks, no blocking. nullptr clears it.
void hw_set_filesystem_generation_hook(void (*hook)());

// Note writes: the encoded payload goes to internal FFat and is mirrored to
// the SD card when it is mounted; the result reflects the internal write.
// `allow_prune` gates the periodic internal-storage eviction sweep that a save
// may trigger. Latency-sensitive callers (e.g. the editor's exit teardown) pass
// false so returning to the caller is never delayed by housekeeping; the sweep
// still fires on the next ordinary save.
bool hw_save_preferred_file(const char *path, const char *content, std::string *error = nullptr,
                            bool allow_prune = true);

// Failure callback for hw_save_preferred_file_async(). Runs on the save
// worker task (not the LVGL thread): only thread-safe calls such as
// core::notify::post().
typedef void (*hw_save_fail_cb_t)(const char *path, const std::string &error);

// IS.2: save a note on a background worker (core 0, low priority) so leaving
// the editor never waits for encode + FFat + SD. `content` is moved into the
// job. At most one save is in flight: a second call first waits for the
// previous one, so writes stay ordered. Every reader/lister/deleter below
// waits for a pending save first, so nothing observes a half-written note.
// If the worker cannot be spawned the save runs synchronously. Returns false
// only when that synchronous fallback failed.
bool hw_save_preferred_file_async(const char *path, std::string content,
                                  hw_save_fail_cb_t on_fail = nullptr);

// Block until a pending background save has finished. Called by the sleep,
// power-off and light-sleep paths so a note is never lost to a power cut.
void hw_save_flush();

// True while a background save is running.
bool hw_save_pending();

// Partial read: stops after `max_bytes` so callers can build previews cheaply.
// `truncated` (if non-null) is set to true when the file was longer than max_bytes.
bool hw_read_preferred_file_snippet(const char *path, std::string &content,
                                    size_t max_bytes, bool *truncated = nullptr);

// Directory listing with modification time; used to build a stable index key.
// Optional callback: (current_count, total_count_if_known, current_filename)
void hw_get_preferred_txt_files_info(std::vector<std::pair<std::string, uint32_t>> &list,
                                     void (*cb)(int, int, const char *) = nullptr);

// Binary I/O on the preferred storage. Needed for on-disk caches/indexes that
// can contain embedded NUL bytes.
bool hw_save_preferred_bytes(const char *path, const uint8_t *buf, size_t len,
                             std::string *error = nullptr);
bool hw_read_preferred_bytes(const char *path, std::vector<uint8_t> &buf);

// When the internal-storage note count reaches 50, move the 35 oldest .txt
// notes to the SD card (leaving 15 on internal as headroom). No-op below the
// threshold or when the SD card is unavailable. Excludes tasks.txt.
// Optional callback: (current_count, total_count, current_filename)
void hw_prune_internal_storage(void (*cb)(int, int, const char *) = nullptr);

// One-shot migration: copies the files in FFat's root plus the notes in
// NOTES_DIR to the same paths on SD.
// `copied` / `failed` receive the counts (pass nullptr to ignore). On failure
// of the whole operation (e.g. SD unavailable) returns false and sets *error.
// Optional callback: (current_count, total_count_if_known, current_filename)
bool hw_copy_internal_to_sd(int *copied, int *failed, std::string *error = nullptr,
                             void (*cb)(int, int, const char *) = nullptr);

// One-shot mirror: uploads every .txt note under /notes on internal FFat *and*
// the SD card to the hub via /api/notes/upload. Same name on both sides → the
// internal copy wins (a just-edited internal note is what gets uploaded even
// if a stale SD copy exists). `copied` / `failed` receive the counts. Returns
// false when the hub is disabled, WiFi is down, or no notes were uploaded
// successfully — in that case `*error` (if non-null) carries a diagnostic.
// Optional callback: (current_count, total_count, current_filename)
bool hw_copy_all_notes_to_hub(int *copied, int *failed,
                              std::string *error = nullptr,
                              void (*cb)(int, int, const char *) = nullptr);
