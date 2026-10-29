/**
 * @file      pbkdf2_sha256.h
 * @brief     Portable PBKDF2-HMAC-SHA256 (RFC 8018 §5.2) with a software SHA-256.
 *
 * Exists because the S3's mbedTLS routes every 64-byte SHA block through the
 * DMA SHA peripheral (per-block lock + setup) and re-keys the HMAC on every
 * iteration, so one notes_crypto derivation cost 80k hardware block ops
 * (PERFORMANCE_PLAN.md P7.2). Here the HMAC ipad/opad midstates are computed
 * once per passphrase, so each iteration costs exactly two software SHA-256
 * compressions. Output is bit-identical to mbedtls_pkcs5_pbkdf2_hmac /
 * `openssl kdf PBKDF2` / Python hashlib.pbkdf2_hmac('sha256', ...).
 *
 * No secret-dependent branches or table lookups: the compression is plain
 * SHA-256 (add/rotate/xor on words). The only branch on secret-derived data is
 * on the passphrase *length* (> 64 B is pre-hashed, per HMAC).
 *
 * Pure C++, no Arduino/IDF dependency, so native_test compiles it.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace hal {

/* HMAC-SHA256 key schedule for one passphrase: the SHA-256 chaining states
 * after absorbing (K ^ ipad) and (K ^ opad). Secret: wipe with secure_wipe()
 * when done. */
struct Pbkdf2Sha256Ctx {
    uint32_t istate[8];
    uint32_t ostate[8];
};

/* Compute the midstates for `pw`. */
void pbkdf2_sha256_init(Pbkdf2Sha256Ctx *ctx, const uint8_t *pw, size_t pw_len);

/* Compute PBKDF2 output block T_`block_index` (1-based, per RFC 8018) into
 * `out`: U_1 = HMAC(P, S || INT(i)), U_j = HMAC(P, U_{j-1}), T = U_1 ^ ... ^ U_c.
 * `ctx` is only read, so different blocks may run concurrently on different
 * cores against the same ctx. Every local intermediate (U, T, message
 * schedule) is wiped before returning. iterations must be >= 1. */
void pbkdf2_sha256_block(const Pbkdf2Sha256Ctx *ctx,
                         const uint8_t *salt, size_t salt_len,
                         uint32_t iterations, uint32_t block_index,
                         uint8_t out[32]);

/* Full sequential PBKDF2-HMAC-SHA256 for any out_len. Returns false only on
 * invalid arguments (iterations == 0, out_len == 0, or out_len too large). */
bool pbkdf2_hmac_sha256(const uint8_t *pw, size_t pw_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t *out, size_t out_len);

/* Streaming SHA-256 state (the same compression as the PBKDF2 above). */
struct Sha256Stream {
    uint32_t st[8];
    uint8_t buf[64];
    size_t n;
    uint64_t total;
    uint32_t blk[16];
    uint32_t W[64];
};

/* Plain SHA-256: init, absorb any number of chunks, then final (which leaves
 * `s` spent; init again to reuse). Used to check OTA images (hal/ota.cpp). */
void sha256_init(Sha256Stream *s);
void sha256_update(Sha256Stream *s, const uint8_t *p, size_t len);
void sha256_final(Sha256Stream *s, uint8_t out[32]);

/* Wipe `n` bytes in a way the compiler cannot elide. The project's one
 * secure-wipe primitive (DC2.4.27b): notes_crypto and hal::secret_scrub()
 * use it too. */
void secure_wipe(void *buf, size_t n);

}  // namespace hal
