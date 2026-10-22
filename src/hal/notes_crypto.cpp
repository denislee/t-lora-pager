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
#include <mbedtls/platform_util.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif

/* The canary sits in its own NVS namespace so schema changes to the main
 * settings blob don't drag it along. */
#define NC_NS           "notes_sec"
#define NC_KEY_CANARY   "canary"
/* Matches `openssl enc -pbkdf2` default. Keep in lockstep with the host sync
 * script; if this changes, the sync script's openssl invocation must gain an
 * explicit `-iter` flag. */
#define NC_ITERATIONS   10000
#define NC_CANARY_PT    "NOTES_OK"
#define NC_SALT_LEN     8
#define NC_KEY_LEN      32
#define NC_IV_LEN       16
#define NC_MAGIC        "Salted__"

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

static void zeroize(void *buf, size_t n)
{
#ifdef ARDUINO
    mbedtls_platform_zeroize(buf, n);
#else
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (n--) *p++ = 0;
#endif
}

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

static std::string display_label(const ProtFile &pf)
{
    return (pf.on_sd ? std::string("SD: ") : std::string("")) + pf.path;
}

static bool raw_read(const ProtFile &pf, std::vector<uint8_t> &out)
{
    out.clear();
#ifdef ARDUINO
    char s[256];
    normalize_path(pf.path.c_str(), s, sizeof(s));
    core::MaybeSpiLock lock(pf.on_sd);
    return hal::fs_read_all(pf.on_sd ? static_cast<fs::FS &>(SD)
                                     : static_cast<fs::FS &>(FFat), s, out);
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
    return hal::fs_write_all(pf.on_sd ? static_cast<fs::FS &>(SD)
                                      : static_cast<fs::FS &>(FFat), s, buf, len);
#else
    (void)pf; (void)buf; (void)len; return true;
#endif
}

/* Protected files on the SD card: the notes/<leaf>.txt list from
 * hw_get_sd_txt_files (the same NOTES_DIR walk the internal side uses via
 * hw_get_internal_txt_files), plus the journal index at the SD root. */
#ifdef ARDUINO
static void collect_sd_protected(std::vector<ProtFile> &out)
{
    if (!(HW_SD_ONLINE & hw_get_device_online())) return;
    std::vector<std::string> txt;
    hw_get_sd_txt_files(txt);
    for (auto &p : txt) {
        if (notes_crypto_path_is_protected(p.c_str())) out.push_back({p, true});
    }
    core::ScopedSpiLock lock;
    if (SD.exists("/journal_idx.bin")) out.push_back({"/journal_idx.bin", true});
}
#endif

static void enumerate_protected(std::vector<ProtFile> &out)
{
    out.clear();
    std::vector<std::string> txt;
    hw_get_internal_txt_files(txt);
    for (auto &p : txt) {
        if (notes_crypto_path_is_protected(p.c_str())) out.push_back({p, false});
    }
#ifdef ARDUINO
    if (FFat.exists("/journal_idx.bin")) {
        out.push_back({"/journal_idx.bin", false});
    }
    /* Try to pick up the card if it wasn't mounted yet — a passphrase set
     * while the card was out should still migrate it on the next rotation. */
    if (!(HW_SD_ONLINE & hw_get_device_online())) {
        hw_mount_sd();
    }
    collect_sd_protected(out);
#endif
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
    if (!(HW_SD_ONLINE & hw_get_device_online())) return false;
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

void notes_crypto_encrypt_existing(void (*cb)(int cur, int total, const char *name))
{
    if (!g_unlocked) return;

    std::vector<ProtFile> files;
    enumerate_protected(files);

    int total = (int)files.size();
    int cur = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());
        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;
        if (has_magic(raw.data(), raw.size())) continue;    /* already encrypted */

        std::vector<uint8_t> ct;
        if (!encrypt_with_pw(g_passphrase.c_str(),
                             raw.data(), raw.size(), ct)) continue;
        raw_write(pf, ct.data(), ct.size());
    }
    if (cb) cb(total, total, "Done");
}

bool notes_crypto_encrypt_sd(int *scanned, int *encrypted,
                             void (*cb)(int cur, int total, const char *name))
{
    if (scanned)   *scanned = 0;
    if (encrypted) *encrypted = 0;
    if (!notes_crypto_is_enabled() || !g_unlocked) return false;

#ifdef ARDUINO
    if (!(HW_SD_ONLINE & hw_get_device_online())) {
        hw_mount_sd();
    }
    if (!(HW_SD_ONLINE & hw_get_device_online())) return false;

    std::vector<ProtFile> files;
    collect_sd_protected(files);

    int total = (int)files.size();
    int cur = 0;
    int enc_count = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());

        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;
        if (has_magic(raw.data(), raw.size())) continue;    /* already encrypted */

        std::vector<uint8_t> ct;
        if (!encrypt_with_pw(g_passphrase.c_str(),
                             raw.data(), raw.size(), ct)) continue;
        if (raw_write(pf, ct.data(), ct.size())) enc_count++;
    }
    if (cb) cb(total, total, "Done");
    if (scanned)   *scanned   = total;
    if (encrypted) *encrypted = enc_count;
    return true;
#else
    (void)cb;
    return false;
#endif
}

bool notes_crypto_change_passphrase(const char *old_pw, const char *new_pw,
                                    void (*cb)(int cur, int total, const char *name))
{
    if (!old_pw || !new_pw || !*new_pw) return false;
    if (!notes_crypto_unlock(old_pw)) return false;

    std::string old_pass = old_pw;
    std::string new_pass = new_pw;

    std::vector<ProtFile> files;
    enumerate_protected(files);

    int total = (int)files.size();
    int cur = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());

        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;

        std::string plain;
        if (has_magic(raw.data(), raw.size())) {
            if (!decrypt_with_pw(old_pass.c_str(),
                                 raw.data(), raw.size(), plain)) continue;
        } else {
            plain.assign((const char *)raw.data(), raw.size());
        }

        std::vector<uint8_t> ct;
        if (!encrypt_with_pw(new_pass.c_str(),
                             (const uint8_t *)plain.data(), plain.size(), ct)) continue;
        raw_write(pf, ct.data(), ct.size());
        zeroize(&plain[0], plain.size());
    }

    /* Rotate the canary last — a crash between file rewrites and the canary
     * update would leave us with mixed keys under the OLD canary, which the
     * user can still recover from. */
    std::vector<uint8_t> canary_ct;
    if (!encrypt_with_pw(new_pass.c_str(),
                         (const uint8_t *)NC_CANARY_PT,
                         strlen(NC_CANARY_PT), canary_ct)) return false;
    if (!write_canary(canary_ct)) return false;

    zeroize(&old_pass[0], old_pass.size());
    {
        PrewarmLock lk;
        g_passphrase = new_pass;
        prewarm_clear_locked();     /* may hold a triple primed under the old key */
#ifdef ARDUINO
        keycache_flush_locked();    /* the rewrite loop cached derivations under the old key */
#endif
        g_unlocked = true;
    }
    zeroize(&new_pass[0], new_pass.size());
    if (cb) cb(total, total, "Done");
    return true;
}

bool notes_crypto_disable(const char *pw, void (*cb)(int cur, int total, const char *name))
{
    if (!pw) return false;
    if (!notes_crypto_unlock(pw)) return false;

    std::vector<ProtFile> files;
    enumerate_protected(files);
    int total = (int)files.size();
    int cur = 0;
    for (const auto &pf : files) {
        cur++;
        std::string label = display_label(pf);
        if (cb) cb(cur, total, label.c_str());

        std::vector<uint8_t> raw;
        if (!raw_read(pf, raw)) continue;
        if (!has_magic(raw.data(), raw.size())) continue;  /* already plain */

        std::string plain;
        if (!decrypt_with_pw(g_passphrase.c_str(),
                             raw.data(), raw.size(), plain)) continue;
        raw_write(pf, (const uint8_t *)plain.data(), plain.size());
        zeroize(&plain[0], plain.size());
    }

    erase_canary();
    notes_crypto_lock();
    if (cb) cb(total, total, "Done");
    return true;
}
