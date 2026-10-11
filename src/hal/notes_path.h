/**
 * @file      notes_path.h
 * @brief     Pure-logic helpers for notes-crypto path/format policy.
 *
 * Extracted from notes_crypto.cpp + storage.cpp so that the rules deciding
 * which files get encrypted (and how to detect already-encrypted bytes) can
 * be exercised by host-side unit tests without pulling in Arduino, mbedtls,
 * or the filesystem layer. Both functions are pure: no I/O, no globals.
 */
#pragma once

#include <cstddef>

namespace hal {
/* Where the Notes app's .txt files live, on both internal FFat and the SD
 * card: NOTES_DIR as an absolute directory, NOTES_PREFIX as the relative
 * prefix of a note path ("notes/<leaf>.txt"). tasks.txt and journal_idx.bin
 * stay at the FFat root — they are bookkeeping, not user notes. */
constexpr const char *NOTES_DIR    = "/notes";
constexpr const char *NOTES_PREFIX = "notes/";
}

/* Write `in` into `out` (capacity `cap`) as an absolute "/path": a leading
 * '/' is added when missing. null or "" yields "". The result is truncated
 * (always NUL-terminated) when it does not fit; cap == 0 writes nothing.
 * Stack-buffer replacement for Arduino's `String("/") + path` in fs paths. */
void normalize_path(const char *in, char *out, size_t cap);

/* Normalize a companion-supplied path (PagerLink fs.*) into an absolute path
 * under the volume root: "/" or "/a/b". An optional leading '/' is accepted,
 * repeated '/' collapse, "." components and a trailing '/' are dropped, and
 * "" yields "/". Rejected (returns false and writes "" when cap > 0): null,
 * any ".." component, a backslash, a byte < 0x20 or 0x7f, a component longer
 * than 64 bytes, or a result that does not fit in `cap` (NUL included). */
bool sandbox_path(const char *in, char *out, size_t cap);

/* True when this path falls under the notes encryption policy:
 *   - Exactly "journal_idx.bin" at the root.
 *   - Top-level "notes/<leaf>.txt" — a single level under notes/, .txt suffix.
 * Anything else (subdirectories of notes/, non-.txt files, paths outside
 * notes/) is left in plaintext. Leading slash on `path` is tolerated. */
bool notes_crypto_path_is_protected(const char *path);

/* True when `buf` begins with the OpenSSL `enc` "Salted__" magic. Used to
 * decide whether on-disk bytes are already ciphertext during read/migrate. */
bool content_has_salted_magic(const char *buf, size_t len);

/* True for the Tasks app's bookkeeping file at the FFat root ("tasks.txt",
 * leading slash tolerated). The journal and the file browser hide it. */
bool notes_path_is_tasks_file(const char *path);
