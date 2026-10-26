/**
 * @file      notes_link.h
 * @brief     Notes sync over the PagerLink companion (notes.sync).
 *
 * The phone holds the GitHub repo/branch/PAT (HUB_REMOVAL_PLAN.md H2.5,
 * D-H2): the pager only announces a manifest of its notes and the phone pulls
 * the bytes it needs with fs.read, then reports progress on evt.notes.sync.
 * Encrypted notes travel as their raw Salted__ ciphertext, so no unlock is
 * needed. This is the only header the notes UIs include for that feature.
 *
 * Blocking: notes_link_sync() drives hw_link_call() and waits for events —
 * run it on a worker task/thread (see the threading note in link.h).
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "notes_manifest.h"
#include "result.h"

// No evt.notes.sync for this long during a job → the sync fails.
constexpr uint32_t NOTES_SYNC_STALL_MS = 120000;

// Totals over every job of one notes_link_sync() run.
struct NotesSyncSummary {
    int uploaded = 0;
    int already = 0;
    int deleted = 0;  // moved: the phone verified them on GitHub and deleted them here
    int kept = 0;     // verified-not or changed since the pull, still here
    std::vector<std::string> errors;  // names the phone reported as failed
};

// Build the manifest from the file-service volumes: every *.txt under /notes
// on "int", then (include_sd and the SD card mounted) on "sd", skipping SD
// names already present internally (internal wins). CRC-32 over the raw
// on-disk bytes. False + err when the internal notes folder can't be listed
// or a file can't be read.
bool notes_link_manifest(bool include_sd, std::vector<NoteEntry> &out, std::string &err);

// UI log sink; called on the worker, one line per progress event.
using NotesLogFn = std::function<void(const std::string &line)>;

// Run notes.sync for `files` (batched, one job per batch) with `targets`
// ("github" or "phone") and wait for each job's done event. Each
// evt.notes.sync is forwarded to `log` as "uploaded=N already=M errors=K"
// (+ the failing names). On failure returns the HalError and fills err with a
// UI-ready message.
HalResult<NotesSyncSummary> notes_link_sync(const std::vector<NoteEntry> &files,
                                            const std::vector<std::string> &targets,
                                            const NotesLogFn &log, std::string &err);

// Settings » Storage "Copy Notes -> Phone": manifest of internal + SD notes
// (internal wins a shared name) sent as notes.sync with targets ["phone"].
HalResult<NotesSyncSummary> notes_link_copy_to_phone(std::string &err);
