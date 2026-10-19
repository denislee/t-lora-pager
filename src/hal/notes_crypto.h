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

/* OpenSSL-`enc` format buffer crypto. Both require the session to be unlocked. */
bool notes_crypto_encrypt_buffer(const uint8_t *pt, size_t pt_len,
                                 std::vector<uint8_t> &out);
bool notes_crypto_decrypt_buffer(const uint8_t *ct, size_t ct_len,
                                 std::string &out);

/* Kick off derivation of one (salt, key, iv) triple so the next encrypt skips
 * PBKDF2. PBKDF2-HMAC-SHA256 x10000 is ~2 s on the S3 (IS.10); priming it at a
 * latency-insensitive moment such as opening the editor means the save that
 * fires when the user *exits* only pays for the AES-CBC pass, not the key
 * derivation. The derivation runs on a short-lived background task, so this
 * returns immediately and never blocks the caller on PBKDF2 (it falls back to
 * an inline derive only if the task can't be spawned). Cheap no-op if locked, a
 * triple is already primed, or one is already being derived. The salt is freshly
 * random and consumed exactly once, so on-disk salt uniqueness (and OpenSSL-
 * `enc` compatibility) is preserved. Call from the UI thread. */
void notes_crypto_prewarm();

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
