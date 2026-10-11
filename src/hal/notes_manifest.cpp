/**
 * @file      notes_manifest.cpp
 * @brief     Implementation of notes_manifest.h — pure, no I/O, no Arduino deps.
 *
 * builds the {targets, files} params payload for one notes.sync REQ and the
 * greedy batching that keeps each payload within NOTES_SYNC_MAX_PARAMS.
 */
#include "notes_manifest.h"

#include "link/link_json.h"

std::string notes_sync_params(const std::vector<std::string> &targets,
                              const std::vector<NoteEntry> &files, size_t begin, size_t end)
{
    pagerlink::JsonWriter w;
    w.begin_object();

    w.key("targets").begin_array();
    for (const std::string &t : targets) w.str_val(t);
    w.end_array();

    w.key("files").begin_array();
    for (size_t i = begin; i < end; ++i) {
        const NoteEntry &f = files[i];
        w.begin_object()
            .key("name")
            .str_val(f.name)
            .key("size")
            .uint_val(f.size)
            .key("mtime")
            .uint_val(f.mtime)
            .key("crc32")
            .uint_val(f.crc32)
            .key("vol")
            .str_val(f.vol)
            .end_object();
    }
    w.end_array();

    w.end_object();
    return w.str();
}

std::vector<std::pair<size_t, size_t>> notes_manifest_batches(
    const std::vector<std::string> &targets, const std::vector<NoteEntry> &files, size_t max_bytes)
{
    std::vector<std::pair<size_t, size_t>> batches;
    const size_t n = files.size();
    if (n == 0) return batches;

    size_t start = 0;
    while (start < n) {
        /* A batch always holds at least one file: extend it one file at a
         * time while the serialized params still fit. */
        size_t end = start + 1;
        while (end < n &&
               notes_sync_params(targets, files, start, end + 1).size() <= max_bytes) {
            ++end;
        }
        batches.emplace_back(start, end);
        start = end;
    }
    return batches;
}
