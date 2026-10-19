/**
 * @file      secrets.h
 * @brief     Encrypted NVS wrapper for device credentials.
 *
 * Stores short secrets (bearer tokens, PATs) as AES-256-CBC ciphertext under
 * caller-chosen (namespace, key) slots in NVS. Crypto is the same PBKDF2 +
 * passphrase session as notes_crypto — one unlock covers every slot. The
 * layout is intentionally identical to what ui_telegram.cpp / ui_notes_sync.cpp
 * wrote by hand, so existing NVS entries keep working.
 */
#pragma once

#include <string>

namespace hal {

/* True if ciphertext exists at (ns, key). Does not require the session to be
 * unlocked — safe to call at boot so UI can decide whether to prompt. */
bool secret_exists(const char *ns, const char *key);

/* Decrypt and return plaintext, scoped to the caller's frame. Returns empty
 * when the slot is missing, the session is locked, or the blob is corrupt.
 * The caller is responsible for scrubbing any copies it holds. */
std::string secret_load(const char *ns, const char *key);

/* Encrypt `plaintext` and persist. With `trim_ws` (the default) leading and
 * trailing whitespace is stripped first: a pasted token often carries a
 * trailing newline/space, and stored as-is it breaks every Authorization
 * header. Pass false for values where whitespace is significant (the SSH
 * password). Passing null, "" (or, trimmed, all-whitespace) clears the slot
 * and succeeds without needing the session unlocked. A non-empty write
 * requires notes_crypto enabled AND unlocked; on refusal returns false with
 * *err set. Everything is validated before NVS is opened read-write. */
bool secret_store(const char *ns, const char *key, const char *plaintext,
                  std::string *err = nullptr, bool trim_ws = true);

/* Masked form of the secret at (ns, key) for Settings labels: "(not set)",
 * "(locked)" when ciphertext exists but the notes session is locked,
 * "********" for a value of 8 characters or fewer, otherwise "********"
 * plus the last four characters. The decrypted copy is scrubbed. */
std::string secret_mask_display(const char *ns, const char *key);

/* Non-blocking variant for the UI thread (P7.3). Fills `out` and returns
 * true when the answer needs no PBKDF2: "(not set)", "(locked)", or a mask
 * remembered this session from an earlier secret_store / secret_mask_display
 * of the same slot. Returns false when only a decrypt can tell; call
 * secret_mask_display off the UI thread then (it remembers the result). */
bool secret_mask_peek(const char *ns, const char *key, std::string &out);

/* Drop a plaintext predecessor key that pre-dates encryption (e.g. the
 * bare "token" slot that lived next to "token_enc" during migration).
 * Idempotent — safe to call on every boot. */
void secret_purge_legacy(const char *ns, const char *legacy_key);

/* Overwrite a std::string's backing bytes before release so decrypted
 * plaintext doesn't linger in freed heap pages. */
void secret_scrub(std::string &s);

} // namespace hal
