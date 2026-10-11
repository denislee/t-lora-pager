/**
 * @file      notes_manifest.h
 * @brief     notes.sync manifest entries, params serialization and batching.
 *
 * Pure (no I/O, no link, no LVGL) so the native Unity env can test it
 * (test/test_notes_manifest). hal/notes_link.cpp builds the entries from the
 * file-service volumes and runs one notes.sync job per batch.
 *
 * A REQ payload is capped at 4096 B (PROTOCOL.md §3), so a long manifest is
 * split into consecutive batches whose serialized params stay within
 * NOTES_SYNC_MAX_PARAMS, leaving headroom for the envelope.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Largest serialized notes.sync params object one batch may produce.
constexpr size_t NOTES_SYNC_MAX_PARAMS = 3500;

// One manifest entry. `name` is the leaf under /notes on `vol` ("int" or
// "sd"); size/mtime/crc32 describe the raw on-disk bytes (ciphertext for an
// encrypted note), exactly what fs.read serves.
struct NoteEntry {
    std::string name;
    uint32_t size = 0;
    uint32_t mtime = 0;
    uint32_t crc32 = 0;
    std::string vol = "int";
};

// Serialize {"targets":[...],"files":[{name,size,mtime,crc32,vol}...]} for
// files[begin, end).
std::string notes_sync_params(const std::vector<std::string> &targets,
                              const std::vector<NoteEntry> &files, size_t begin, size_t end);

// Split `files` into consecutive [begin, end) ranges, in order, each covering
// at least one file, such that notes_sync_params() for every range is at most
// `max_bytes` (a single file that alone exceeds it still gets its own range).
// An empty list yields no ranges.
std::vector<std::pair<size_t, size_t>> notes_manifest_batches(
    const std::vector<std::string> &targets, const std::vector<NoteEntry> &files,
    size_t max_bytes = NOTES_SYNC_MAX_PARAMS);
