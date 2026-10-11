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
 * (notes_path.h): "/" or "/a/b", no "..", no trailing '/'. Each call opens and
 * closes its file; nothing is held between calls.
 */
#pragma once

#include <cstddef>
#include <cstdint>
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

}  // namespace pagerlink
