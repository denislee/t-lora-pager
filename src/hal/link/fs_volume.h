/**
 * @file      fs_volume.h
 * @brief     Raw volume I/O for the PagerLink file service (handlers_fs.cpp).
 *
 * The file service moves on-disk bytes as they are: no notes-crypto decode or
 * encode, so an encrypted note leaves the pager as its Salted__ ciphertext
 * (IOS_COMPANION_PLAN.md §8). Two volumes:
 *  - Vol::Int — internal FFat on hardware.
 *  - Vol::Sd  — the SD card on hardware (SPI bus held per call through
 *               core::ScopedSpiLock); unavailable while it is not mounted.
 *
 * On the emulator both volumes are real directories, `$PAGERLINK_FS_ROOT/int`
 * and `$PAGERLINK_FS_ROOT/sd` (default root /tmp/pagerlink-fs), created on
 * first use. They are independent of the emulator's canned storage listings
 * that the UI and the screenshot sweep render, so link transfers can be tested
 * end to end without changing any screenshot.
 *
 * Every `path` is an absolute path already passed through sandbox_path()
 * (notes_path.h): "/" or "/a/b", no "..", no trailing '/'. Each vol_* call
 * opens and closes its file. A VolFile (below) keeps one file open across the
 * chunks of a bulk transfer instead.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pagerlink {

enum class Vol : uint8_t { Int, Sd };

/** "int" → Vol::Int, "sd" → Vol::Sd; anything else is false. */
bool vol_from_string(const std::string &s, Vol &out);

struct VolEntry {
    std::string name; /* leaf name ("" for the root) */
    bool dir;
    uint32_t size;  /* bytes; 0 for directories */
    uint32_t mtime; /* unix seconds; 0 when unknown */
};

/** True when the volume can be used now (the SD card is mounted). */
bool vol_available(Vol v);

/**
 * List the directory `dir`: folders first, then files, each sorted by name
 * (the file browser's order). List entries may report mtime 0 (a per-entry
 * stat is slow on FAT). False when `dir` is not a directory.
 */
bool vol_list(Vol v, const std::string &dir, std::vector<VolEntry> &out);

/** Stat one path, with its mtime when the filesystem knows it. False when missing. */
bool vol_stat(Vol v, const std::string &path, VolEntry &out);

/** Read exactly `n` bytes at `offset`; false when the file is shorter or unreadable. */
bool vol_read_at(Vol v, const std::string &path, uint32_t offset, uint8_t *buf, size_t n);

/**
 * Write `n` bytes at `offset`. offset 0 creates or truncates the file (n may
 * be 0); any other offset must equal the current file size (append). False on
 * any short write or an offset that is not the current size.
 */
bool vol_write_at(Vol v, const std::string &path, uint32_t offset, const uint8_t *buf, size_t n);

/** Remove a file, or a directory with everything under it. Never the root. */
bool vol_remove(Vol v, const std::string &path);

/** Create one directory level; true if it already exists as a directory. */
bool vol_mkdir(Vol v, const std::string &path);

/** Rename/move within the volume. False when `to` already exists. */
bool vol_rename(Vol v, const std::string &from, const std::string &to);

/** Free bytes on the volume; false when unknown. */
bool vol_free_bytes(Vol v, uint64_t &out);

/**
 * One file kept open across the chunks of a transfer (P7.34), so a chunk costs
 * a read/write instead of stat + open + seek + close (and, for writes, a
 * FAT/directory sync per chunk). Each call takes the volume lock (SD: the SPI
 * bus) for that chunk only; nothing is held between calls.
 *
 * Which volumes really stay open is the implementation's call: on hardware the
 * internal FFat does; the SD card reopens per call, because the vendor
 * lightSleep() unmounts and remounts it and a FILE* that outlived the mount
 * could alias a file opened after the remount. A closed file (close(), or
 * one closed by its owner before a delete/rename of its path) reopens itself on
 * the next read_at/write_at, by path, with the same checks vol_read_at /
 * vol_write_at make. The destructor closes.
 */
class VolFile {
public:
    VolFile();
    ~VolFile();
    VolFile(const VolFile &) = delete;
    VolFile &operator=(const VolFile &) = delete;

    /** Bind to a file that must exist and be a regular file; opens it. */
    bool open_read(Vol v, const std::string &path);
    /** Create or truncate `path` and keep it open for appends from offset 0. */
    bool open_write(Vol v, const std::string &path);
    /** Exactly `n` bytes at `offset` (any offset; seeks only when needed). */
    bool read_at(uint32_t offset, uint8_t *buf, size_t n);
    /** Append: `offset` must equal the bytes written so far (the file size). */
    bool write_at(uint32_t offset, const uint8_t *buf, size_t n);
    /** Release the handle (and, for writes, sync size + FAT). Safe to repeat. */
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Whole-file CRC-32/ISO-HDLC of a file whose fresh vol_stat() gave `size` and
 * `mtime`, from a small cache keyed by (vol, path, size, mtime, filesystem
 * generation) or else computed (one open file, MAX_DATA_CHUNK reads) and
 * stored. Thread-safe (link_task and the notes-sync worker both call it); the
 * cache lock is never held across I/O. Nothing is stored while a background
 * note save is pending or when the generation moved during the pass. Writes
 * through this layer (vol_write_at, VolFile writes) forget their path, and
 * vol_remove / vol_rename forget everything, because they bump no generation.
 */
bool vol_file_crc(Vol v, const std::string &path, uint32_t size, uint32_t mtime, uint32_t &out);

/** Drop the cached CRC of (v, path): a streamed read found it stale. */
void vol_crc_forget(Vol v, const std::string &path);

}  // namespace pagerlink
