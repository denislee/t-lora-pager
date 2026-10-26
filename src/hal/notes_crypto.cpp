/**
 * @file      notes_crypto.cpp
 * @brief     OpenSSL-compatible AES-256-CBC / PBKDF2-HMAC-SHA256 for notes.
 */
#include "notes_crypto.h"
#include "notes_path.h"
#include "fs_util.h"
#include "storage.h"
#include "../hal_interface.h"
#include "../core/spi_lock.h"
#include "pbkdf2_sha256.h"
#include "notes_migrate.h"
#include "secrets.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <new>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <LilyGoLib.h>
#include <FFat.h>
#include <SD.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif

#ifdef ARDUINO  // used only by the hardware paths (emulator -Wunused-macros)
/* The canary sits in its own NVS namespace so schema changes to the main
 * settings blob don't drag it along. */
#define NC_NS           "notes_sec"
#define NC_KEY_CANARY   "canary"
/* Matches `openssl enc -pbkdf2` default. Keep in lockstep with the host sync
 * script; if this changes, the sync script's openssl invocation must gain an
 * explicit `-iter` flag. */
#define NC_ITERATIONS   10000
#define NC_MAGIC        "Salted__"
#endif
#define NC_CANARY_PT    "NOTES_OK"
#define NC_SALT_LEN     8
#define NC_KEY_LEN      32
#define NC_IV_LEN       16

namespace {

bool g_unlocked = false;
std::string g_passphrase;   /* Held in RAM while unlocked. */

/* Key-material generation. Bumped (under prewarm_mutex) by every cache flush,
 * i.e. on every lock, unlock and passphrase change. Anything that derives a
 * key outside the mutex (decrypt, the prewarm task, the pin path, the unlock
 * verify) snapshots it first and only publishes if it is unchanged, so a
 * derivation that straddles a lock() can never leave key material behind it
 * or re-open the session. */
uint32_t g_key_epoch = 0;

/* One pre-derived (salt, key, iv) triple, primed by notes_crypto_prewarm() and
 * consumed by the next encrypt so it can skip the ~10k-iteration PBKDF2. Bound
 * to the passphrase it was derived under so a mid-session rotation can't hand a
 * stale key to encrypt. Consumed exactly once — the salt is never reused. */
struct PrewarmSlot {
    bool valid = false;
    std::string pw;
    uint8_t salt[NC_SALT_LEN];
    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];
};
PrewarmSlot g_prewarm;

/* g_prewarm is filled by a background derivation task (see notes_crypto_prewarm)
 * and consumed/cleared by the UI/save thread, so every access is serialised
 * through this mutex. PrewarmLock is a no-op on the emulator, which has no
 * background task. */
#ifdef ARDUINO
static SemaphoreHandle_t prewarm_mutex()
{
    /* C++11 thread-safe static init: created exactly once, matching the
     * notes_fs_mutex() idiom in storage.cpp. */
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
struct PrewarmLock {
    PrewarmLock()  { xSemaphoreTake(prewarm_mutex(), portMAX_DELAY); }
    ~PrewarmLock() { xSemaphoreGive(prewarm_mutex()); }
    PrewarmLock(const PrewarmLock &) = delete;
    PrewarmLock &operator=(const PrewarmLock &) = delete;
};
/* At most one derivation task in flight; set on the UI thread before spawn,
 * cleared by the worker as it exits (same discipline as s_prune_task_running). */
static std::atomic<bool> s_prewarm_running{false};
#else
struct PrewarmLock { PrewarmLock() {} ~PrewarmLock() {} };
#endif

static inline void zeroize(void *buf, size_t n) { hal::secure_wipe(buf, n); }

/* Wipe the primed slot. Callers must hold prewarm_mutex(). */
static void prewarm_clear_locked()
{
    if (g_prewarm.valid) {
        zeroize(g_prewarm.salt, sizeof(g_prewarm.salt));
        zeroize(g_prewarm.key, sizeof(g_prewarm.key));
        zeroize(g_prewarm.iv, sizeof(g_prewarm.iv));
    }
    if (!g_prewarm.pw.empty()) zeroize(&g_prewarm.pw[0], g_prewarm.pw.size());
    g_prewarm.pw.clear();
    g_prewarm.valid = false;
}

/* ---- Read-side key cache ----
 * PBKDF2 (10k iterations) costs 1.9-2.7 s per derivation on the S3 (measured,
 * INPUT_STUTTER_PLAN.md IS.10). Encrypt always uses a fresh random salt, which
 * is then read back: the journal previews a note by decrypting the whole file,
 * then the user opens the same note, then maybe reopens it — each decrypts the
 * identical on-disk ciphertext and would otherwise re-derive the same key.
 * Cache the derived (key, iv) keyed on the 8-byte salt so repeat decrypts of an
 * unchanged file skip PBKDF2; encrypt seeds the cache with the salt it just
 * wrote so the first read-back after a save hits too.
 *
 * The cache holds live key material, so it is treated exactly like the cached
 * passphrase: entries are only ever stored for the current session key, the
 * whole cache is zeroized in notes_crypto_lock(), and it is flushed on every
 * passphrase change. Because every entry therefore belongs to the one current
 * passphrase, keying on salt alone is sufficient. Shares prewarm_mutex() because
 * decrypt runs on both the UI thread and the journal's background rescan task.
 * MRU is kept at index 0; valid entries stay packed at the front. */
#ifdef ARDUINO
struct KeyCacheEntry {
    bool valid = false;
    uint8_t salt[NC_SALT_LEN];
    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];
};
constexpr int NC_KEYCACHE_N = 6;
KeyCacheEntry g_keycache[NC_KEYCACHE_N];

/* Pinned set (P6.22b): one slot per fixed-salt blob that is read far more
 * often than it is written (the journal index, the secret slots; tags in
 * notes_crypto.h). Same entry type and the same rules as the LRU (session key
 * only, flushed with it), but outside the LRU so six note previews can't evict
 * them. Separate slots rather than a bigger LRU because a bigger LRU still
 * gives no guarantee; cost is NC_PIN_N x sizeof(KeyCacheEntry) = 4 x 57 B of
 * .bss. A slot is replaced when its blob is rewritten (new salt), so it never
 * holds more than one key per blob. */
KeyCacheEntry g_pinned[NC_PIN_N];

static void entry_wipe(KeyCacheEntry &e)
{
    zeroize(e.salt, NC_SALT_LEN);
    zeroize(e.key, NC_KEY_LEN);
    zeroize(e.iv,  NC_IV_LEN);
    e.valid = false;
}

/* Callers must hold prewarm_mutex(). Returns the cached derivation for `salt`
 * (pinned set first) and promotes an LRU hit to MRU. */
static bool keycache_lookup_locked(const uint8_t salt[NC_SALT_LEN],
                                   uint8_t key_out[NC_KEY_LEN],
                                   uint8_t iv_out[NC_IV_LEN])
{
    for (int i = 0; i < NC_PIN_N; i++) {
        if (!g_pinned[i].valid) continue;
        if (memcmp(g_pinned[i].salt, salt, NC_SALT_LEN) != 0) continue;
        memcpy(key_out, g_pinned[i].key, NC_KEY_LEN);
        memcpy(iv_out,  g_pinned[i].iv,  NC_IV_LEN);
        return true;
    }
    for (int i = 0; i < NC_KEYCACHE_N; i++) {
        if (!g_keycache[i].valid) continue;
        if (memcmp(g_keycache[i].salt, salt, NC_SALT_LEN) != 0) continue;
        memcpy(key_out, g_keycache[i].key, NC_KEY_LEN);
        memcpy(iv_out,  g_keycache[i].iv,  NC_IV_LEN);
        if (i != 0) {   /* promote to front (block-shift the more-recent entries) */
            KeyCacheEntry hit = g_keycache[i];
            for (int j = i; j > 0; j--) g_keycache[j] = g_keycache[j - 1];
            g_keycache[0] = hit;
        }
        return true;
    }
    return false;
}

/* Callers must hold prewarm_mutex(). Inserts (salt, key, iv) at MRU, evicting
 * and zeroizing the LRU entry when full. No-op if the salt is already cached. */
static void keycache_store_locked(const uint8_t salt[NC_SALT_LEN],
                                  const uint8_t key[NC_KEY_LEN],
                                  const uint8_t iv[NC_IV_LEN])
{
    for (int i = 0; i < NC_PIN_N; i++) {
        if (g_pinned[i].valid &&
            memcmp(g_pinned[i].salt, salt, NC_SALT_LEN) == 0) return;
    }
    for (int i = 0; i < NC_KEYCACHE_N; i++) {
        if (g_keycache[i].valid &&
            memcmp(g_keycache[i].salt, salt, NC_SALT_LEN) == 0) return;
    }
    /* Drop the key material about to fall off the end, then shift down. */
    zeroize(g_keycache[NC_KEYCACHE_N - 1].key, NC_KEY_LEN);
    zeroize(g_keycache[NC_KEYCACHE_N - 1].iv,  NC_IV_LEN);
    for (int j = NC_KEYCACHE_N - 1; j > 0; j--) g_keycache[j] = g_keycache[j - 1];
    g_keycache[0].valid = true;
    memcpy(g_keycache[0].salt, salt, NC_SALT_LEN);
    memcpy(g_keycache[0].key,  key,  NC_KEY_LEN);
    memcpy(g_keycache[0].iv,   iv,   NC_IV_LEN);
}

/* Callers must hold prewarm_mutex(). Replaces pinned slot `tag`. */
static void pinned_set_locked(int tag, const uint8_t salt[NC_SALT_LEN],
                              const uint8_t key[NC_KEY_LEN],
                              const uint8_t iv[NC_IV_LEN])
{
    entry_wipe(g_pinned[tag]);
    memcpy(g_pinned[tag].salt, salt, NC_SALT_LEN);
    memcpy(g_pinned[tag].key,  key,  NC_KEY_LEN);
    memcpy(g_pinned[tag].iv,   iv,   NC_IV_LEN);
    g_pinned[tag].valid = true;
}

/* Callers must hold prewarm_mutex(). Drops every derived key, pinned ones
 * included, and starts a new key epoch (see g_key_epoch). */
static void keycache_flush_locked()
{
    for (int i = 0; i < NC_KEYCACHE_N; i++) {
        if (!g_keycache[i].valid) continue;
        zeroize(g_keycache[i].key, NC_KEY_LEN);
        zeroize(g_keycache[i].iv,  NC_IV_LEN);
        g_keycache[i].valid = false;
    }
    for (int i = 0; i < NC_PIN_N; i++) entry_wipe(g_pinned[i]);
    g_key_epoch++;
}
#endif

static bool has_magic(const uint8_t *buf, size_t n)
{
    return content_has_salted_magic((const char *)buf, n);
}

/* ---- NVS canary helpers ---- */

static bool read_canary(std::vector<uint8_t> &out)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, true)) return false;
    size_t len = p.getBytesLength(NC_KEY_CANARY);
    if (len == 0) { p.end(); return false; }
    out.resize(len);
    p.getBytes(NC_KEY_CANARY, out.data(), len);
    p.end();
    return true;
#else
    (void)out; return false;
#endif
}

static bool write_canary(const std::vector<uint8_t> &ct)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, false)) return false;
    p.putBytes(NC_KEY_CANARY, ct.data(), ct.size());
    p.end();
    return true;
#else
    (void)ct; return false;
#endif
}

static void erase_canary()
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, false)) return;
    p.remove(NC_KEY_CANARY);
    p.end();
#endif
}

/* ---- Crypto primitives ---- */

#ifdef ARDUINO
/* PBKDF2 output block T2 (bytes 32..63; the IV is its first 16) computed on a
 * short-lived helper task on the other core while the caller computes T1.
 * The job lives on the caller's stack; the helper touches it only until it
 * gives `done`, and pbkdf2_sha256_block wipes its own locals before that, so
 * the helper's freed stack holds no intermediates. */
struct Pbkdf2T2Job {
    const hal::Pbkdf2Sha256Ctx *ctx;
    const uint8_t *salt;
    uint8_t out[32];
    StaticSemaphore_t done_buf;
    SemaphoreHandle_t done;
};

static void pbkdf2_t2_task(void *arg)
{
    Pbkdf2T2Job *job = (Pbkdf2T2Job *)arg;
    hal::pbkdf2_sha256_block(job->ctx, job->salt, NC_SALT_LEN, NC_ITERATIONS, 2,
                             job->out);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

/* PBKDF2-HMAC-SHA256(pw, salt, NC_ITERATIONS, 48) -> key = T1, iv = T2[0..16),
 * bit-identical to the mbedtls_pkcs5_pbkdf2_hmac call it replaced (P7.2) and
 * to `openssl enc -pbkdf2`; pinned by test/test_pbkdf2. Software SHA-256 with
 * precomputed HMAC midstates (2 compressions per iteration) instead of the
 * S3's per-block DMA SHA path, with T1 and T2 on both cores. If the helper
 * task can't be created (low heap) T2 is computed inline: same output, only
 * slower. */
static bool derive_key_iv(const char *pw, const uint8_t salt[NC_SALT_LEN],
                          uint8_t key_out[NC_KEY_LEN], uint8_t iv_out[NC_IV_LEN])
{
    static_assert(NC_KEY_LEN == 32 && NC_IV_LEN <= 32,
                  "derive_key_iv maps key to T1 and iv to the head of T2");

    hal::Pbkdf2Sha256Ctx ctx;
    uint8_t t1[32];
    Pbkdf2T2Job job;
    hal::pbkdf2_sha256_init(&ctx, (const uint8_t *)pw, strlen(pw));
    job.ctx  = &ctx;
    job.salt = salt;
    job.done = xSemaphoreCreateBinaryStatic(&job.done_buf);

    bool helper = false;
#if !CONFIG_FREERTOS_UNICORE
    if (job.done) {
        const BaseType_t other_core = xPortGetCoreID() == 0 ? 1 : 0;
        helper = xTaskCreatePinnedToCore(pbkdf2_t2_task, "pbkdf2_t2", 4096, &job,
                                         uxTaskPriorityGet(nullptr), nullptr,
                                         other_core) == pdPASS;
    }
#endif

    hal::pbkdf2_sha256_block(&ctx, salt, NC_SALT_LEN, NC_ITERATIONS, 1, t1);
    if (helper) {
        xSemaphoreTake(job.done, portMAX_DELAY);
    } else {
        hal::pbkdf2_sha256_block(&ctx, salt, NC_SALT_LEN, NC_ITERATIONS, 2, job.out);
    }
    if (job.done) vSemaphoreDelete(job.done);

    memcpy(key_out, t1, NC_KEY_LEN);
    memcpy(iv_out,  job.out, NC_IV_LEN);
    zeroize(&ctx, sizeof(ctx));
    zeroize(t1, sizeof(t1));
    zeroize(job.out, sizeof(job.out));
    return true;
}
#endif

static bool encrypt_with_pw(const char *pw,
                            const uint8_t *pt, size_t pt_len,
                            std::vector<uint8_t> &out)
{
#ifdef ARDUINO
    uint8_t salt[NC_SALT_LEN];
    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];

    /* Consume a matching pre-derived triple if one is primed for this exact
     * passphrase; otherwise derive synchronously. Either way the salt is fresh
     * and used once. The check/copy/clear runs under the prewarm mutex because
     * the background derivation task may be publishing into the slot right now. */
    bool consumed = false;
    uint32_t epoch;
    {
        PrewarmLock lk;
        epoch = g_key_epoch;
        if (g_prewarm.valid && g_prewarm.pw == pw) {
            memcpy(salt, g_prewarm.salt, NC_SALT_LEN);
            memcpy(key, g_prewarm.key, NC_KEY_LEN);
            memcpy(iv, g_prewarm.iv, NC_IV_LEN);
            prewarm_clear_locked();
            consumed = true;
        }
    }
    if (!consumed) {
        esp_fill_random(salt, NC_SALT_LEN);
        if (!derive_key_iv(pw, salt, key, iv)) return false;
    }

    /* PKCS7 pad to 16-byte block. */
    size_t pad = NC_IV_LEN - (pt_len % NC_IV_LEN);
    size_t body_len = pt_len + pad;
    std::vector<uint8_t> body(body_len);
    if (pt_len) memcpy(body.data(), pt, pt_len);
    for (size_t i = 0; i < pad; i++) body[pt_len + i] = (uint8_t)pad;

    std::vector<uint8_t> ct(body_len);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = false;
    if (mbedtls_aes_setkey_enc(&ctx, key, 256) == 0) {
        uint8_t iv_work[NC_IV_LEN];
        memcpy(iv_work, iv, NC_IV_LEN);
        ok = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, body_len,
                                   iv_work, body.data(), ct.data()) == 0;
        zeroize(iv_work, sizeof(iv_work));
    }
    mbedtls_aes_free(&ctx);
    /* Seed the read-side cache with this fresh salt (P7.1): the next read of the
     * file just written (reopen after edit, journal snippet, index reload) would
     * otherwise pay a full PBKDF2 for a key we already hold. Same gate as the
     * decrypt side, so only live-session keys are ever cached. */
    if (ok) {
        PrewarmLock lk;
        if (g_unlocked && g_key_epoch == epoch && g_passphrase == pw)
            keycache_store_locked(salt, key, iv);
    }
    zeroize(key, sizeof(key));
    zeroize(iv, sizeof(iv));
    zeroize(body.data(), body.size());
    /* Re-arm (P6.22a): the primed triple is single-use, so the next save in
     * the same editing session would otherwise pay the full derive again.
     * No-op unless unlocked; any thread. */
    if (consumed) notes_crypto_prewarm();
    if (!ok) return false;

    out.clear();
    out.reserve(8 + NC_SALT_LEN + body_len);
    out.insert(out.end(), (const uint8_t *)NC_MAGIC, (const uint8_t *)NC_MAGIC + 8);
    out.insert(out.end(), salt, salt + NC_SALT_LEN);
    out.insert(out.end(), ct.begin(), ct.end());
    return true;
#else
    (void)pw;
    out.assign(pt, pt + pt_len);
    return true;
#endif
}

static bool decrypt_with_pw(const char *pw,
                            const uint8_t *ct, size_t ct_len,
                            std::string &out)
{
#ifdef ARDUINO
    /* Minimum: 8 magic + 8 salt + 1 block. */
    if (ct_len < 8 + NC_SALT_LEN + NC_IV_LEN) return false;
    if (!has_magic(ct, ct_len)) return false;
    size_t body_len = ct_len - 8 - NC_SALT_LEN;
    if (body_len == 0 || (body_len % NC_IV_LEN) != 0) return false;

    const uint8_t *salt = ct + 8;
    const uint8_t *body = ct + 8 + NC_SALT_LEN;

    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];

    /* Reuse a cached (key, iv) for this salt when decrypting under the live
     * session key. Only cache while `pw` is the current session passphrase: the
     * canary probe in notes_crypto_unlock() decrypts a fixed salt under arbitrary
     * candidate passphrases, which must never poison the salt-keyed cache. */
    bool cacheable = false;
    bool have_key = false;
    uint32_t epoch = 0;
    {
        PrewarmLock lk;
        cacheable = g_unlocked && (g_passphrase == pw);
        epoch = g_key_epoch;
        if (cacheable) have_key = keycache_lookup_locked(salt, key, iv);
    }
    if (!have_key) {
        if (!derive_key_iv(pw, salt, key, iv)) return false;
        if (cacheable) {
            PrewarmLock lk;
            /* A lock()/unlock that ran during the derive flushed the cache;
             * don't put this key back behind it. */
            if (g_unlocked && g_key_epoch == epoch) keycache_store_locked(salt, key, iv);
        }
    }

    std::vector<uint8_t> plain(body_len);
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = false;
    if (mbedtls_aes_setkey_dec(&ctx, key, 256) == 0) {
        uint8_t iv_work[NC_IV_LEN];
        memcpy(iv_work, iv, NC_IV_LEN);
        ok = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, body_len,
                                   iv_work, body, plain.data()) == 0;
        zeroize(iv_work, sizeof(iv_work));
    }
    mbedtls_aes_free(&ctx);
    zeroize(key, sizeof(key));
    zeroize(iv, sizeof(iv));
    if (!ok) {
        zeroize(plain.data(), plain.size());
        return false;
    }

    /* Strip PKCS7 pad with constant-ish checks. */
    uint8_t pad = plain.back();
    if (pad == 0 || pad > NC_IV_LEN || pad > body_len) {
        zeroize(plain.data(), plain.size());
        return false;
    }
    for (size_t i = 0; i < pad; i++) {
        if (plain[body_len - 1 - i] != pad) {
            zeroize(plain.data(), plain.size());
            return false;
        }
    }
    out.assign((const char *)plain.data(), body_len - pad);
    zeroize(plain.data(), plain.size());
    return true;
#else
    (void)pw;
    out.assign((const char *)ct, ct_len);
    return true;
#endif
}

/* ---- Raw FS helpers used by the migration routines.
 * These deliberately bypass the auto-crypto wrappers in hal/storage.cpp so we
 * can see the raw on-disk ciphertext during passphrase rotation. */

struct ProtFile {
    std::string path;
    bool on_sd;         /* false = FFat internal, true = SD card */
};

static std::string display_label(const nc_migrate::NameCopies &n)
{
    return (!n.ffat && n.sd ? std::string("SD: ") : std::string("")) + n.path;
}

#ifdef ARDUINO
static fs::FS &vol_fs(bool on_sd)
{
    return on_sd ? static_cast<fs::FS &>(SD) : static_cast<fs::FS &>(FFat);
}
#endif

static bool raw_read(const ProtFile &pf, std::vector<uint8_t> &out)
{
    out.clear();
#ifdef ARDUINO
    char s[256];
    normalize_path(pf.path.c_str(), s, sizeof(s));
    core::MaybeSpiLock lock(pf.on_sd);
    return hal::fs_read_all(vol_fs(pf.on_sd), s, out);
#else
    (void)pf; return false;
#endif
}

static bool raw_write(const ProtFile &pf, const uint8_t *buf, size_t len)
{
#ifdef ARDUINO
    char s[256];
    normalize_path(pf.path.c_str(), s, sizeof(s));
    core::MaybeSpiLock lock(pf.on_sd);
    return hal::fs_write_all(vol_fs(pf.on_sd), s, buf, len);
#else
    (void)pf; (void)buf; (void)len; return true;
#endif
}

/* P6.27: read only the 8-byte magic. 1 = OpenSSL `enc` blob, 0 = plaintext
 * (or shorter than the magic), -1 = could not open. */
static int raw_probe(const ProtFile &pf)
{
#ifdef ARDUINO
    char s[256];
    normalize_path(pf.path.c_str(), s, sizeof(s));
    core::MaybeSpiLock lock(pf.on_sd);
    File f = vol_fs(pf.on_sd).open(s, FILE_READ);
    if (!f) return -1;
    uint8_t hdr[8];
    size_t n = f.read(hdr, sizeof(hdr));
    f.close();
    return has_magic(hdr, n) ? 1 : 0;
#else
    (void)pf; return -1;
#endif
}

static bool raw_exists(const ProtFile &pf)
{
#ifdef ARDUINO
    char s[256];
    normalize_path(pf.path.c_str(), s, sizeof(s));
    core::MaybeSpiLock lock(pf.on_sd);
    return vol_fs(pf.on_sd).exists(s);
#else
    (void)pf; return false;
#endif
}

static bool raw_remove(const ProtFile &pf)
{
#ifdef ARDUINO
    char s[256];
    normalize_path(pf.path.c_str(), s, sizeof(s));
    core::MaybeSpiLock lock(pf.on_sd);
    fs::FS &fs = vol_fs(pf.on_sd);
    if (!fs.exists(s)) return true;
    return fs.remove(s);
#else
    (void)pf; return true;
#endif
}

/* Protected names on FFat and/or SD, grouped by name (P7.7): notes/<leaf>.txt
 * from hw_get_internal_txt_files / hw_get_sd_txt_files (same NOTES_DIR walk,
 * same path form) plus the journal index at each root. The SD card is mounted
 * first if it wasn't, so a passphrase set while the card was out still
 * migrates it on the next rotation. */
static void enumerate_names(std::vector<nc_migrate::NameCopies> &out,
                            bool want_ffat, bool want_sd)
{
    out.clear();
    std::vector<std::string> f, s;
    if (want_ffat) {
        std::vector<std::string> txt;
        hw_get_internal_txt_files(txt);
        for (auto &p : txt) {
            if (notes_crypto_path_is_protected(p.c_str())) f.push_back(p);
        }
#ifdef ARDUINO
        if (FFat.exists("/journal_idx.bin")) f.push_back("/journal_idx.bin");
#endif
    }
#ifdef ARDUINO
    if (want_sd) {
        if (!hw_sd_online()) hw_mount_sd();
        if (hw_sd_online()) {
            std::vector<std::string> txt;
            hw_get_sd_txt_files(txt);
            for (auto &p : txt) {
                if (notes_crypto_path_is_protected(p.c_str())) s.push_back(p);
            }
            core::ScopedSpiLock lock;
            if (SD.exists("/journal_idx.bin")) s.push_back("/journal_idx.bin");
        }
    }
#else
    (void)want_sd;
#endif
    out = nc_migrate::group_by_name(f, s);
}

/* The selected copies of one name. Either buffer may hold note plaintext, so
 * both are wiped on the way out. */
struct Copies {
    std::vector<uint8_t> f, s;
    bool f_ok = false, s_ok = false;
    ~Copies()
    {
        if (!f.empty()) zeroize(f.data(), f.size());
        if (!s.empty()) zeroize(s.data(), s.size());
    }
};

/* Read the selected copies of `n` and say how they relate. Byte equality is
 * checked on the two buffers just read (size, then memcmp), which is exact;
 * a size+CRC comparison would need the same two reads. */
static nc_migrate::CopyPlan load_copies(const nc_migrate::NameCopies &n,
                                        bool want_f, bool want_s, Copies &c)
{
    c.f_ok = want_f && n.ffat && raw_read({n.path, false}, c.f);
    c.s_ok = want_s && n.sd && raw_read({n.path, true}, c.s);
    bool same = c.f_ok && c.s_ok &&
                nc_migrate::bytes_equal(c.f.data(), c.f.size(), c.s.data(), c.s.size());
    return nc_migrate::plan_copies(c.f_ok, c.s_ok, same);
}

/* ---- NVS state of the change-passphrase protocol (namespace NC_NS). */
#define NC_KEY_PENDING  "pending"   /* enc(new_pw, NC_CANARY_PT) while a change is in flight */
#define NC_KEY_PEND_SD  "pend_sd"   /* u8: the in-flight change may have staged files on SD */
#define NC_KEY_STG_SEC  "stg_s"     /* + slot index: staged secret ciphertext */

static bool nvs_get(const char *key, std::vector<uint8_t> &out)
{
    out.clear();
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, true)) return false;
    size_t len = p.getBytesLength(key);
    if (len > 0) {
        out.resize(len);
        if (p.getBytes(key, out.data(), len) != len) out.clear();
    }
    p.end();
    return !out.empty();
#else
    (void)key; return false;
#endif
}

[[maybe_unused]] static bool nvs_put(const char *key, const uint8_t *buf, size_t len)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, false)) return false;
    bool ok = p.putBytes(key, buf, len) == len;
    p.end();
    return ok;
#else
    (void)key; (void)buf; (void)len; return false;
#endif
}

static void nvs_del(const char *key)
{
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NC_NS, false)) return;
    if (p.isKey(key)) p.remove(key);
    p.end();
#else
    (void)key;
#endif
}

static std::string stg_sec_key(size_t i)
{
    char k[16];
    snprintf(k, sizeof(k), NC_KEY_STG_SEC "%u", (unsigned)i);
    return k;
}

static bool pending_touches_sd()
{
    std::vector<uint8_t> v;
    return nvs_get(NC_KEY_PEND_SD, v) && v[0] != 0;
}

/* Install (roll_forward) or delete (!roll_forward) every staged copy and
 * staged secret left by a change-passphrase. File/NVS moves only, no crypto.
 * True when nothing staged is left behind, i.e. the pending marker may go.
 * A staged copy is installed by rewriting the original in place and only then
 * removing the staged file, so the original never disappears and an
 * interruption here is simply resumed by the next unlock. */
static bool settle_staged(bool roll_forward,
                          const std::vector<nc_migrate::NameCopies> *known = nullptr)
{
    bool complete = true;
    std::vector<nc_migrate::NameCopies> walked;
    if (!known) {
        enumerate_names(walked, true, true);
        known = &walked;
    }
    if (pending_touches_sd() && !hw_sd_online()) complete = false;   /* retry with the card in */

    for (const auto &n : *known) {
        for (int v = 0; v < 2; v++) {
            bool on_sd = (v == 1);
            if (on_sd ? !n.sd : !n.ffat) continue;
            ProtFile st{nc_migrate::staged_path(n.path), on_sd};
            if (!raw_exists(st)) continue;
            if (roll_forward) {
                std::vector<uint8_t> ct;
                if (!raw_read(st, ct) || !has_magic(ct.data(), ct.size()) ||
                    !raw_write({n.path, on_sd}, ct.data(), ct.size())) {
                    complete = false;
                    continue;
                }
            }
            if (!raw_remove(st)) complete = false;
        }
    }

    for (size_t i = 0; i < hal::secret_slot_count(); i++) {
        std::string k = stg_sec_key(i);
        std::vector<uint8_t> ct;
        if (!nvs_get(k.c_str(), ct)) continue;
        if (roll_forward && !hal::secret_slot_write_raw(i, ct.data(), ct.size())) {
            complete = false;
            continue;
        }
        nvs_del(k.c_str());
    }
    return complete;
}

static void clear_pending()
{
    nvs_del(NC_KEY_PEND_SD);
    nvs_del(NC_KEY_PENDING);
}

/* Called by notes_crypto_unlock after `pw` passed the canary, before the
 * session is published: finish (the commit happened) or undo (it didn't) an
 * interrupted change-passphrase. See "Change-passphrase ordering" below. */
static void recover_pending_change(const char *pw)
{
    std::vector<uint8_t> pend;
    if (!nvs_get(NC_KEY_PENDING, pend)) return;
    std::string dec;
    bool verifies = decrypt_with_pw(pw, pend.data(), pend.size(), dec) &&
                    dec == NC_CANARY_PT;
    if (!dec.empty()) zeroize(&dec[0], dec.size());
    bool roll_forward =
        nc_migrate::pending_recovery(true, verifies) == nc_migrate::Recovery::RollForward;
    printf("[notes_crypto] interrupted passphrase change: %s\n",
           roll_forward ? "installing staged copies" : "discarding staged copies");
    if (settle_staged(roll_forward)) clear_pending();
    else printf("[notes_crypto] staged copies left; retrying on next unlock\n");
}

} /* namespace */

/* ===== Public API ===== */

bool notes_crypto_is_enabled()
{
    std::vector<uint8_t> c;
    return read_canary(c);
}

bool notes_crypto_is_unlocked()
{
    return g_unlocked;
}

bool notes_crypto_should_encrypt()
{
    /* Writes only ever produce ciphertext when the session is unlocked. If
     * encryption is enabled but we're locked, the caller must fail hard rather
     * than silently leak plaintext — see hal/storage.cpp. */
    return g_unlocked;
}

bool notes_crypto_unlock(const char *pw)
{
    if (!pw) return false;
    uint32_t epoch;
    {
        PrewarmLock lk;
        epoch = g_key_epoch;
    }
    std::vector<uint8_t> canary;
    if (!read_canary(canary)) return false;

    std::string dec;
    bool ok = decrypt_with_pw(pw, canary.data(), canary.size(), dec) &&
              dec == NC_CANARY_PT;
    if (!dec.empty()) zeroize(&dec[0], dec.size());
    if (!ok) return false;

    /* P7.7: an interrupted change-passphrase is finished or undone here, before
     * anything can read notes under this session. No-op (one NVS lookup) when
     * nothing is pending. */
    recover_pending_change(pw);

    /* Publish only after the canary verified, and atomically with the flush.
     * May run on a worker (ui_lock.cpp's unlock job), so it is done under the
     * mutex every reader of the cache takes; if a lock() or another unlock
     * landed while we were deriving, the epoch moved and we leave the session
     * as that call left it. */
    PrewarmLock lk;
    if (g_key_epoch != epoch) return false;
    prewarm_clear_locked();
#ifdef ARDUINO
    keycache_flush_locked();    /* stale entries from any prior session */
#endif
    g_passphrase = pw;
    g_unlocked = true;
    return true;
}

/* One observer, notified once the session is fully locked. Runs on the task
 * that called lock(), outside prewarm_mutex; the hook must not touch crypto
 * state (the app hooks only post to the LVGL thread). */
static std::atomic<void (*)()> g_lock_hook{nullptr};

void notes_crypto_set_lock_hook(void (*hook)())
{
    g_lock_hook.store(hook);
}

void notes_crypto_lock()
{
    /* Mark locked and drop the pre-derived key atomically: a background prewarm
     * task that is mid-derive checks g_unlocked under this same mutex before
     * publishing, so it discards its result rather than re-priming a slot the
     * user just tried to lock. */
    {
        PrewarmLock lk;
        g_unlocked = false;
        prewarm_clear_locked();
#ifdef ARDUINO
        keycache_flush_locked();    /* every derived key, pinned ones included */
#endif
        /* Under the mutex too: unlock and the pin path read g_passphrase
         * under it from worker threads. */
        if (!g_passphrase.empty()) {
            zeroize(&g_passphrase[0], g_passphrase.size());
        }
        g_passphrase.clear();
    }
    /* P7.25: tell the one observer, with the session already locked and the
     * mutex released. */
    if (void (*hook)() = g_lock_hook.load()) hook();
}

bool notes_crypto_encrypt_buffer(const uint8_t *pt, size_t pt_len,
                                 std::vector<uint8_t> &out)
{
    if (!g_unlocked) return false;
    return encrypt_with_pw(g_passphrase.c_str(), pt, pt_len, out);
}

#ifdef ARDUINO
/* Derive a fresh (salt, key, iv) triple for `pw` and publish it into the slot,
 * unless the session locked or another primer beat us to it in the meantime.
 * The ~10k-iteration PBKDF2 is the expensive part and runs before the lock is
 * taken, so the mutex is held only for the memcpy that publishes the result. */
static void prewarm_derive_and_publish(const std::string &pw, uint32_t epoch)
{
    uint8_t salt[NC_SALT_LEN];
    esp_fill_random(salt, NC_SALT_LEN);
    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];

    if (derive_key_iv(pw.c_str(), salt, key, iv)) {
        PrewarmLock lk;
        /* Only publish while still unlocked and unprimed: a lock() that raced us
         * has already flipped g_unlocked under this same mutex, so we drop the
         * derived key rather than leave material behind. */
        if (g_unlocked && g_key_epoch == epoch && !g_prewarm.valid) {
            memcpy(g_prewarm.salt, salt, NC_SALT_LEN);
            memcpy(g_prewarm.key, key, NC_KEY_LEN);
            memcpy(g_prewarm.iv, iv, NC_IV_LEN);
            g_prewarm.pw = pw;
            g_prewarm.valid = true;
        }
    }
    zeroize(salt, sizeof(salt));
    zeroize(key, sizeof(key));
    zeroize(iv, sizeof(iv));
}

/* One-shot worker that runs PBKDF2 off the UI thread. The passphrase arrives as
 * a private heap copy (the worker never touches g_passphrase) and is zeroized
 * before the task exits. */
struct PrewarmArg {
    std::string pw;
    uint32_t epoch;
};

static void prewarm_task(void *arg)
{
    PrewarmArg *a = static_cast<PrewarmArg *>(arg);
    prewarm_derive_and_publish(a->pw, a->epoch);
    if (!a->pw.empty()) zeroize(&a->pw[0], a->pw.size());
    delete a;
    s_prewarm_running = false;
    vTaskDelete(nullptr);
}
#endif

void notes_crypto_prewarm()
{
#ifdef ARDUINO
    if (!g_unlocked) return;
    /* At most one derivation in flight; claimed atomically so a re-arm from a
     * save worker (encrypt_with_pw) and the editor can't both spawn one. */
    if (s_prewarm_running.exchange(true)) return;

    /* Snapshot the passphrase (and the epoch it belongs to) under the mutex so
     * the worker can never race a concurrent lock()/rotation of g_passphrase;
     * the worker owns and zeroizes this copy. */
    PrewarmArg *a = new (std::nothrow) PrewarmArg();
    if (!a) { s_prewarm_running = false; return; }
    {
        PrewarmLock lk;
        if (!g_unlocked || g_prewarm.valid) {   /* locked, or already primed */
            delete a;
            s_prewarm_running = false;
            return;
        }
        a->pw = g_passphrase;
        a->epoch = g_key_epoch;
    }

    /* Derive on a low-priority background task so opening the editor never blocks
     * on PBKDF2. If FreeRTOS can't spawn it, derive inline as a fallback — that
     * path pays the full derive, but only when task slots are exhausted. */
    if (xTaskCreate(prewarm_task, "notes_prewarm", 8192, a, 1, nullptr) != pdPASS) {
        prewarm_derive_and_publish(a->pw, a->epoch);
        if (!a->pw.empty()) zeroize(&a->pw[0], a->pw.size());
        delete a;
        s_prewarm_running = false;
    }
#endif
}

bool notes_crypto_pin_key(int tag, const uint8_t *ct, size_t ct_len, bool derive)
{
#ifdef ARDUINO
    if (tag < 0 || tag >= NC_PIN_N) return false;
    if (!ct || ct_len < 8 + NC_SALT_LEN || !has_magic(ct, ct_len)) return false;
    uint8_t salt[NC_SALT_LEN];
    memcpy(salt, ct + 8, NC_SALT_LEN);
    uint8_t key[NC_KEY_LEN];
    uint8_t iv[NC_IV_LEN];
    std::string pw;
    uint32_t epoch;
    {
        PrewarmLock lk;
        if (!g_unlocked) return false;
        if (keycache_lookup_locked(salt, key, iv)) {
            pinned_set_locked(tag, salt, key, iv);
            zeroize(key, sizeof(key));
            zeroize(iv, sizeof(iv));
            return true;
        }
        if (!derive) return false;
        pw = g_passphrase;      /* private copy; wiped below */
        epoch = g_key_epoch;
    }
    bool ok = derive_key_iv(pw.c_str(), salt, key, iv);
    if (!pw.empty()) zeroize(&pw[0], pw.size());
    if (ok) {
        PrewarmLock lk;
        ok = g_unlocked && g_key_epoch == epoch;
        if (ok) pinned_set_locked(tag, salt, key, iv);
    }
    zeroize(key, sizeof(key));
    zeroize(iv, sizeof(iv));
    return ok;
#else
    (void)tag; (void)ct; (void)ct_len; (void)derive;
    return false;
#endif
}

void notes_crypto_unpin_key(int tag)
{
#ifdef ARDUINO
    if (tag < 0 || tag >= NC_PIN_N) return;
    PrewarmLock lk;
    entry_wipe(g_pinned[tag]);
#else
    (void)tag;
#endif
}

#ifdef ARDUINO
/* First 16 bytes (magic + salt) of a root file, FFat first then SD — the
 * same order hw_read_preferred_bytes uses, without reading the whole index. */
static bool read_preferred_header(const char *path, uint8_t out[8 + NC_SALT_LEN])
{
    const size_t n = 8 + NC_SALT_LEN;
    File f = FFat.open(path, FILE_READ);
    if (f) {
        bool ok = f.read(out, n) == n;
        f.close();
        return ok;
    }
    if (!hw_sd_online()) return false;
    core::ScopedSpiLock lock;
    f = SD.open(path, FILE_READ);
    if (!f) return false;
    bool ok = f.read(out, n) == n;
    f.close();
    return ok;
}
#endif

void notes_crypto_prewarm_fixed()
{
#ifdef ARDUINO
    if (!g_unlocked) return;
    uint8_t hdr[8 + NC_SALT_LEN];
    if (read_preferred_header("/journal_idx.bin", hdr)) {
        notes_crypto_pin_key(NC_PIN_JOURNAL_INDEX, hdr, sizeof(hdr), /*derive=*/true);
    }
    hal::secret_prewarm_pinned();
#endif
}

#ifdef ARDUINO
static std::atomic<bool> s_prewarm_fixed_running{false};

static void prewarm_fixed_task(void *)
{
    notes_crypto_prewarm_fixed();
    s_prewarm_fixed_running = false;
    vTaskDelete(nullptr);
}
#endif

void notes_crypto_prewarm_fixed_async()
{
#ifdef ARDUINO
    if (!g_unlocked || s_prewarm_fixed_running.exchange(true)) return;
    /* Priority 1, below the LVGL task: a foreground derive (opening a note)
     * preempts this one. pin_key's epoch check drops keys if a lock() races. */
    if (xTaskCreate(prewarm_fixed_task, "nc_prewarm_fx", 8192, nullptr, 1, nullptr) != pdPASS) {
        s_prewarm_fixed_running = false;
    }
#endif
}

bool notes_crypto_decrypt_buffer(const uint8_t *ct, size_t ct_len,
                                 std::string &out)
{
    if (!g_unlocked) return false;
    return decrypt_with_pw(g_passphrase.c_str(), ct, ct_len, out);
}

bool notes_crypto_set_passphrase(const char *new_pw)
{
    if (!new_pw || !*new_pw) return false;
    /* Refuse if already enabled — caller should use change_passphrase. */
    std::vector<uint8_t> existing;
    if (read_canary(existing)) return false;

    std::vector<uint8_t> canary_ct;
    if (!encrypt_with_pw(new_pw,
                         (const uint8_t *)NC_CANARY_PT,
                         strlen(NC_CANARY_PT), canary_ct)) return false;
    if (!write_canary(canary_ct)) return false;

    PrewarmLock lk;
    prewarm_clear_locked();
#ifdef ARDUINO
    keycache_flush_locked();
#endif
    g_passphrase = new_pw;
    g_unlocked = true;
    return true;
}

/* Session passphrase copy for a bulk operation running on a worker. Empty when
 * locked. Read under the mutex because unlock/lock write it from other tasks. */
static std::string session_passphrase()
{
    PrewarmLock lk;
    return g_unlocked ? g_passphrase : std::string();
}

static inline void scrub(std::string &s) { hal::secret_scrub(s); }

/* Encrypt the plaintext copies of every protected name under `pw` (P7.7):
 * the 8-byte probe skips copies that are already `enc` blobs without reading
 * them, and a name whose FFat and SD copies are byte-identical is encrypted
 * once and the same ciphertext (same salt, as hw_save_preferred_* would write)
 * goes to both. Returns the number of copies rewritten. */
static int encrypt_plain_names(const std::string &pw, bool want_ffat, bool want_sd,
                               int *scanned, void (*cb)(int cur, int total, const char *name))
{
    std::vector<nc_migrate::NameCopies> names;
    enumerate_names(names, want_ffat, want_sd);
    int total = (int)names.size();
    if (scanned) *scanned = total;
    int cur = 0, written = 0;
    for (const auto &n : names) {
        cur++;
        if (cb) {
            std::string label = display_label(n);
            cb(cur, total, label.c_str());
        }
        bool wf = n.ffat && raw_probe({n.path, false}) == 0;
        bool ws = n.sd && raw_probe({n.path, true}) == 0;
        if (!wf && !ws) continue;

        Copies c;
        nc_migrate::CopyPlan plan = load_copies(n, wf, ws, c);
        /* A write raced the probe: never wrap an `enc` blob a second time. */
        if (c.f_ok && has_magic(c.f.data(), c.f.size())) c.f_ok = false;
        if (c.s_ok && has_magic(c.s.data(), c.s.size())) c.s_ok = false;
        plan = nc_migrate::plan_copies(c.f_ok, c.s_ok, plan == nc_migrate::CopyPlan::Shared);

        std::vector<uint8_t> ct;
        switch (plan) {
        case nc_migrate::CopyPlan::Shared:
            if (encrypt_with_pw(pw.c_str(), c.f.data(), c.f.size(), ct)) {
                if (raw_write({n.path, false}, ct.data(), ct.size())) written++;
                if (raw_write({n.path, true},  ct.data(), ct.size())) written++;
            }
            break;
        case nc_migrate::CopyPlan::Separate:
        case nc_migrate::CopyPlan::FfatOnly:
        case nc_migrate::CopyPlan::SdOnly:
            if (c.f_ok && encrypt_with_pw(pw.c_str(), c.f.data(), c.f.size(), ct) &&
                raw_write({n.path, false}, ct.data(), ct.size())) written++;
            if (c.s_ok && encrypt_with_pw(pw.c_str(), c.s.data(), c.s.size(), ct) &&
                raw_write({n.path, true}, ct.data(), ct.size())) written++;
            break;
        case nc_migrate::CopyPlan::None:
            break;
        }
    }
    if (cb) cb(total, total, "Done");
    return written;
}

void notes_crypto_encrypt_existing(void (*cb)(int cur, int total, const char *name))
{
    std::string pw = session_passphrase();
    if (pw.empty()) return;
    encrypt_plain_names(pw, true, true, nullptr, cb);
    scrub(pw);
}

bool notes_crypto_encrypt_sd(int *scanned, int *encrypted,
                             void (*cb)(int cur, int total, const char *name))
{
    if (scanned)   *scanned = 0;
    if (encrypted) *encrypted = 0;
    if (!notes_crypto_is_enabled()) return false;

#ifdef ARDUINO
    std::string pw = session_passphrase();
    if (pw.empty()) return false;
    if (!hw_sd_online()) hw_mount_sd();
    if (!hw_sd_online()) { scrub(pw); return false; }

    int n = encrypt_plain_names(pw, false, true, scanned, cb);
    if (encrypted) *encrypted = n;
    scrub(pw);
    return true;
#else
    (void)cb;
    return false;
#endif
}

/* ---- Change-passphrase ordering (P7.7).
 *
 * Invariant: at no point after an interruption (crash, power loss, brown-out)
 * is a protected file or secret encrypted under a passphrase that the canary
 * does not verify. Rewriting files in place cannot give that with one canary:
 * whichever of {files, canary} goes first, a crash in the middle leaves some
 * files under a key the canary rejects (the old code rewrote files first, so an
 * interrupted change left the already-rewritten notes unreadable until the
 * user happened to repeat the same change; that repeat then relied on a wrong-
 * key decrypt failing its PKCS#7 check, which passes by chance ~1/256 per
 * file and would have re-encrypted garbage). So the change is staged and the
 * canary write is the single commit point:
 *
 *   0. NVS `pending` = enc(new, canary text) (+ `pend_sd`). Marks the change.
 *   1. Stage: every protected copy is decrypted under old and its ciphertext
 *      under new written to `<path>.ncnew` next to it (originals untouched);
 *      every secret slot's re-encryption goes to NVS `stg_s<i>`. Any write
 *      failure discards the staged set and fails the change: nothing changed.
 *   2. Commit: canary = enc(new). NVS replaces a blob atomically.
 *   3. Switch the session to new, install the staged copies (rewrite the
 *      original in place, then delete the staged file), clear `pending`.
 *
 * An unlock always settles an interrupted change before publishing the
 * session (recover_pending_change): the canary verifies the passphrase typed;
 * if `pending` verifies it too, step 2 happened and step 3 is resumed; if not,
 * the canary is still the old one and the staged copies are deleted. Either
 * way every original is under the canary's passphrase throughout. Costs: one
 * extra PBKDF2 at an unlock only while `pending` exists, and room for one
 * staged copy of every protected file during the change (a full volume fails
 * the change cleanly at step 1).
 *
 * Not covered (unchanged from before): a note written by another task (phone
 * fs.write, notes sync, an editor save) between its staging and step 3 is
 * overwritten by the staged copy; a copy that does not decrypt under old is
 * left as it is. */
bool notes_crypto_change_passphrase(const char *old_pw, const char *new_pw,
                                    void (*cb)(int cur, int total, const char *name))
{
    if (!old_pw || !new_pw || !*new_pw) return false;
    /* Verifies old and settles any interrupted change first. */
    if (!notes_crypto_unlock(old_pw)) return false;

#ifdef ARDUINO
    std::string old_pass = old_pw;
    std::string new_pass = new_pw;
    struct ScrubBoth {
        std::string &a, &b;
        ~ScrubBoth() { scrub(a); scrub(b); }
    } scrub_both{old_pass, new_pass};

    /* A settle that could not finish (SD card out) must complete first: a new
     * change would otherwise overwrite its `pending` marker. */
    std::vector<uint8_t> existing;
    if (nvs_get(NC_KEY_PENDING, existing)) {
        printf("[notes_crypto] change refused: an interrupted change is still pending\n");
        return false;
    }

    std::vector<nc_migrate::NameCopies> names;
    enumerate_names(names, true, true);
    /* Staged copies left by a rollback that ran without the SD card. */
    settle_staged(false, &names);

    /* Step 0. */
    std::vector<uint8_t> marker;
    if (!encrypt_with_pw(new_pass.c_str(), (const uint8_t *)NC_CANARY_PT,
                         strlen(NC_CANARY_PT), marker)) return false;
    uint8_t any_sd = 0;
    for (const auto &n : names) any_sd |= n.sd ? 1 : 0;
    if (!nvs_put(NC_KEY_PEND_SD, &any_sd, 1) ||
        !nvs_put(NC_KEY_PENDING, marker.data(), marker.size())) {
        clear_pending();
        return false;
    }

    /* Step 1. Decrypt each name once; byte-identical FFat/SD copies share one
     * new ciphertext. */
    const int total = (int)names.size() + 1;    /* + the secret slots */
    int cur = 0;
    bool failed = false;
    auto to_plain = [&](const std::vector<uint8_t> &raw, std::string &plain) -> bool {
        if (has_magic(raw.data(), raw.size()))
            return decrypt_with_pw(old_pass.c_str(), raw.data(), raw.size(), plain);
        plain.assign((const char *)raw.data(), raw.size());   /* plaintext gets encrypted too */
        return true;
    };
    auto stage_copy = [&](const nc_migrate::NameCopies &n, const std::vector<uint8_t> &raw,
                          bool to_ffat, bool to_sd) -> bool {
        std::string plain;
        if (!to_plain(raw, plain)) {
            printf("[notes_crypto] %s: not under the current passphrase, left as is\n",
                   n.path.c_str());
            scrub(plain);
            return true;
        }
        std::vector<uint8_t> ct;
        bool ok = encrypt_with_pw(new_pass.c_str(), (const uint8_t *)plain.data(),
                                  plain.size(), ct);
        scrub(plain);
        std::string st = nc_migrate::staged_path(n.path);
        if (ok && to_ffat) ok = raw_write({st, false}, ct.data(), ct.size());
        if (ok && to_sd)   ok = raw_write({st, true},  ct.data(), ct.size());
        return ok;
    };

    for (const auto &n : names) {
        cur++;
        if (cb) {
            std::string label = display_label(n);
            cb(cur, total, label.c_str());
        }
        Copies c;
        switch (load_copies(n, true, true, c)) {
        case nc_migrate::CopyPlan::Shared:
            failed = !stage_copy(n, c.f, true, true);
            break;
        case nc_migrate::CopyPlan::Separate:
            failed = !stage_copy(n, c.f, true, false) || !stage_copy(n, c.s, false, true);
            break;
        case nc_migrate::CopyPlan::FfatOnly:
            failed = !stage_copy(n, c.f, true, false);
            break;
        case nc_migrate::CopyPlan::SdOnly:
            failed = !stage_copy(n, c.s, false, true);
            break;
        case nc_migrate::CopyPlan::None:
            break;
        }
        if (failed) break;
    }

    /* Secrets: before P7.7 they were not touched here and stopped decrypting
     * after every passphrase change. */
    if (!failed) {
        cur++;
        if (cb) cb(cur, total, "Secrets");
        for (size_t i = 0; i < hal::secret_slot_count() && !failed; i++) {
            std::vector<uint8_t> ct;
            if (!hal::secret_slot_read_raw(i, ct)) continue;
            std::string plain;
            if (!decrypt_with_pw(old_pass.c_str(), ct.data(), ct.size(), plain)) {
                printf("[notes_crypto] secret %s: not under the current passphrase, left as is\n",
                       hal::secret_slot_name(i).c_str());
                continue;
            }
            std::vector<uint8_t> nct;
            bool ok = encrypt_with_pw(new_pass.c_str(), (const uint8_t *)plain.data(),
                                      plain.size(), nct);
            scrub(plain);
            std::string k = stg_sec_key(i);
            if (!ok || !nvs_put(k.c_str(), nct.data(), nct.size())) failed = true;
        }
    }

    std::vector<uint8_t> canary_ct;
    if (!failed && !encrypt_with_pw(new_pass.c_str(), (const uint8_t *)NC_CANARY_PT,
                                    strlen(NC_CANARY_PT), canary_ct)) failed = true;
    if (failed) {
        printf("[notes_crypto] change failed while staging; nothing changed\n");
        if (settle_staged(false, &names)) clear_pending();
        return false;
    }

    /* Step 2: the commit. If NVS reports a failure, read back to learn which
     * canary is actually stored, and settle the staged set to match it. */
    if (!nvs_put(NC_KEY_CANARY, canary_ct.data(), canary_ct.size())) {
        std::vector<uint8_t> now;
        bool committed = read_canary(now) && now == canary_ct;
        if (!committed) {
            if (settle_staged(false, &names)) clear_pending();
            return false;
        }
    }

    /* Step 3. */
    {
        PrewarmLock lk;
        g_passphrase = new_pass;
        prewarm_clear_locked();     /* may hold a triple primed under the old key */
        keycache_flush_locked();    /* old-key derivations; also wipes every pin */
        g_unlocked = true;
    }
    if (cb) cb(total, total, "Finishing...");
    if (settle_staged(true, &names)) clear_pending();
    else printf("[notes_crypto] staged copies left; the next unlock installs them\n");

    /* The flush wiped the pinned keys (journal index, secrets): re-pin them
     * under the new passphrase so the next Journal/Telegram open doesn't derive
     * on the LVGL thread. One PBKDF2 per pinned blob; we are on a worker. */
    notes_crypto_prewarm_fixed();
    if (cb) cb(total, total, "Done");
    return true;
#else
    (void)cb;
    return false;
#endif
}

/* Disable: every protected copy back to plaintext (decrypted once per name
 * when the FFat and SD copies are identical), then the secret slots are
 * ERASED, then the canary. Secrets are never stored in plaintext (secret_store
 * refuses without encryption; secrets.h), and with no canary nothing could
 * ever unlock them again, so erasing is the only state consistent with
 * "encryption off": Settings then shows "(not set)" instead of a "(locked)"
 * that no passphrase opens. Ordering: an interrupted disable leaves plaintext
 * copies (always readable) next to copies under the canary's passphrase, and
 * secrets go before the canary so none outlives the canary that opens it. */
NotesDisableResult notes_crypto_disable(const char *pw,
                                        void (*cb)(int cur, int total, const char *name))
{
    if (!pw) return NotesDisableResult::WrongPassphrase;
    if (!notes_crypto_unlock(pw)) return NotesDisableResult::WrongPassphrase;  /* settles a pending change too */
    /* A change the unlock could not settle (its staged copies or originals are
     * on an SD card that is out) must finish first: erasing the canary now
     * would leave the SD originals under a passphrase nothing asks for again. */
    {
        std::vector<uint8_t> pend;
        if (nvs_get(NC_KEY_PENDING, pend)) {
            printf("[notes_crypto] disable refused: an interrupted change is still pending\n");
            return NotesDisableResult::Pending;
        }
    }
    std::string pass = pw;
    bool write_failed = false;

    std::vector<nc_migrate::NameCopies> names;
    enumerate_names(names, true, true);
    int total = (int)names.size();
    int cur = 0;
    for (const auto &n : names) {
        cur++;
        if (cb) {
            std::string label = display_label(n);
            cb(cur, total, label.c_str());
        }
        bool wf = n.ffat && raw_probe({n.path, false}) == 1;
        bool ws = n.sd && raw_probe({n.path, true}) == 1;
        if (!wf && !ws) continue;                   /* already plain */

        Copies c;
        nc_migrate::CopyPlan plan = load_copies(n, wf, ws, c);
        std::string plain;
        switch (plan) {
        case nc_migrate::CopyPlan::Shared:
            if (decrypt_with_pw(pass.c_str(), c.f.data(), c.f.size(), plain)) {
                if (!raw_write({n.path, false}, (const uint8_t *)plain.data(), plain.size()))
                    write_failed = true;
                if (!raw_write({n.path, true},  (const uint8_t *)plain.data(), plain.size()))
                    write_failed = true;
            }
            break;
        case nc_migrate::CopyPlan::Separate:
        case nc_migrate::CopyPlan::FfatOnly:
        case nc_migrate::CopyPlan::SdOnly:
            if (c.f_ok && decrypt_with_pw(pass.c_str(), c.f.data(), c.f.size(), plain) &&
                !raw_write({n.path, false}, (const uint8_t *)plain.data(), plain.size()))
                write_failed = true;
            scrub(plain);
            if (c.s_ok && decrypt_with_pw(pass.c_str(), c.s.data(), c.s.size(), plain) &&
                !raw_write({n.path, true}, (const uint8_t *)plain.data(), plain.size()))
                write_failed = true;
            break;
        case nc_migrate::CopyPlan::None:
            break;
        }
        scrub(plain);
    }
    scrub(pass);

    /* Security review of P7.7: a copy that is still ciphertext because its
     * plaintext write failed (full or read-only volume) would be unreadable
     * once the canary is gone. Keep encryption on; a retry resumes, since
     * the copies already rewritten are skipped as plain. */
    if (write_failed) {
        printf("[notes_crypto] disable stopped: a plaintext write failed; encryption stays on\n");
        if (cb) cb(total, total, "Failed");
        return NotesDisableResult::WriteFailed;
    }

    for (size_t i = 0; i < hal::secret_slot_count(); i++) hal::secret_slot_erase(i);
    clear_pending();
    erase_canary();
    notes_crypto_lock();
    if (cb) cb(total, total, "Done");
    return NotesDisableResult::Ok;
}
