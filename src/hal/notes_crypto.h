/**
 * @file      notes_crypto.h
 * @brief     Passphrase-based at-rest encryption for user notes.
 *
 * Ciphertext is written in OpenSSL `enc` format:
 *   "Salted__" || salt[8] || AES-256-CBC(PKCS7(plaintext))
 * Key+IV are derived via PBKDF2-HMAC-SHA256 with 10000 iterations (matches
 * `openssl enc -pbkdf2` defaults), so host-side decryption is a one-liner.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* True if a passphrase has been set (verifier present in NVS). */
bool notes_crypto_is_enabled();

/* True if the session is currently unlocked (passphrase cached in RAM). */
bool notes_crypto_is_unlocked();

/* Auto-encrypt gate: enabled && unlocked. Writes short-circuit to plaintext
 * when this returns false. */
bool notes_crypto_should_encrypt();

/* notes_crypto_path_is_protected() — the encryption-policy predicate — is
 * declared in notes_path.h (its definition lives in notes_path.cpp). */

/* Verify `pw` against the stored canary. On success, caches the passphrase
 * for the session. */
bool notes_crypto_unlock(const char *pw);

/* Zeroise and drop the cached passphrase. */
void notes_crypto_lock();

/* P7.25: called at the end of every notes_crypto_lock(), on the locking
 * task, with no crypto lock held; one hook, nullptr to clear. */
void notes_crypto_set_lock_hook(void (*hook)());

/* OpenSSL-`enc` format buffer crypto. Both require the session to be unlocked. */
bool notes_crypto_encrypt_buffer(const uint8_t *pt, size_t pt_len,
                                 std::vector<uint8_t> &out);
bool notes_crypto_decrypt_buffer(const uint8_t *ct, size_t ct_len,
                                 std::string &out);

/* Kick off derivation of one (salt, key, iv) triple so the next encrypt skips
 * PBKDF2. PBKDF2-HMAC-SHA256 x10000 measured ~2 s on the S3 via mbedTLS (IS.10);
 * P7.2's software/dual-core derive is expected to be several times faster (an
 * estimate: device time not yet measured), but still worth hiding. Priming it at a
 * latency-insensitive moment such as opening the editor means the save that
 * fires when the user *exits* only pays for the AES-CBC pass, not the key
 * derivation. The derivation runs on a short-lived background task, so this
 * returns immediately and never blocks the caller on PBKDF2 (it falls back to
 * an inline derive only if the task can't be spawned). Cheap no-op if locked, a
 * triple is already primed, or one is already being derived. The salt is freshly
 * random and consumed exactly once, so on-disk salt uniqueness (and OpenSSL-
 * `enc` compatibility) is preserved. Call from the UI thread. */
void notes_crypto_prewarm();

/* Pinned keys for fixed-salt blobs (P6.22b / IS.10 option b). A blob that is
 * read much more often than it is rewritten keeps one salt between writes, so
 * its derived key can stay resident: these slots sit outside the 6-entry LRU
 * (nothing evicts them) and are flushed with it on lock/unlock/passphrase
 * change. Tags: */
enum : int {
    NC_PIN_JOURNAL_INDEX = 0,   /* /journal_idx.bin (ui_journal.cpp) */
    NC_PIN_SECRET_FIRST  = 1,   /* secrets.cpp kPinnedSlots, in table order */
    NC_PIN_N             = 4,
};

/* Pin the session key for the salt of OpenSSL-`enc` blob `ct` (only its
 * 16-byte header is read) into slot `tag`, replacing what the slot held.
 * Taken from the cache when present (e.g. just seeded by the encrypt that
 * wrote `ct`); otherwise derived only when `derive` is true, which blocks
 * for one PBKDF2 — call that form from a worker. False if locked, `ct` is not
 * an `enc` blob, the key wasn't available, or the session changed meanwhile.
 * Any thread. */
bool notes_crypto_pin_key(int tag, const uint8_t *ct, size_t ct_len, bool derive);

/* Drop pinned slot `tag` (its blob was deleted). Any thread. */
void notes_crypto_unpin_key(int tag);

/* Derive and pin every fixed-salt key that exists on this device: the
 * journal index (FFat first, then SD, like hw_read_preferred_bytes) and the
 * secret slots (hal::secret_prewarm_pinned). One PBKDF2 per blob present;
 * BLOCKS — call it from a worker right after notes_crypto_unlock() succeeds
 * (ui_lock.cpp's unlock job). No-op when locked. */
void notes_crypto_prewarm_fixed();

/* First-time setup: refuses if crypto is already enabled. Writes the canary
 * and unlocks the session. The caller is responsible for migrating any
 * existing plaintext files afterwards. */
bool notes_crypto_set_passphrase(const char *new_pw);

/* Rotates the passphrase. Verifies `old_pw`, re-encrypts every protected file
 * on the internal filesystem under the new passphrase, rewrites the canary.
 * Session ends up unlocked with `new_pw`. */
bool notes_crypto_change_passphrase(const char *old_pw, const char *new_pw,
                                    void (*cb)(int cur, int total, const char *name) = nullptr);

/* Decrypts every protected file back to plaintext, removes the canary, locks
 * the session. */
bool notes_crypto_disable(const char *pw,
                          void (*cb)(int cur, int total, const char *name) = nullptr);

/* Migration helper: after set_passphrase, walk every protected plaintext file
 * and rewrite it encrypted. Safe to call on a mix — files already carrying the
 * Salted__ magic are skipped. */
void notes_crypto_encrypt_existing(void (*cb)(int cur, int total, const char *name) = nullptr);

/* SD-only variant of the above. Mounts the SD card if necessary, then
 * encrypts any protected plaintext *.txt / journal index files found on it.
 * Requires the session to be unlocked; returns false if crypto is disabled,
 * locked, or the SD card cannot be brought online. On success, `scanned` and
 * `encrypted` report how many files were checked and how many were actually
 * rewritten. */
bool notes_crypto_encrypt_sd(int *scanned, int *encrypted,
                             void (*cb)(int cur, int total, const char *name) = nullptr);
