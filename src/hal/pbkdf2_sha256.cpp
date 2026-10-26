/**
 * @file      pbkdf2_sha256.cpp
 * @brief     Portable PBKDF2-HMAC-SHA256 with a software SHA-256 (see header).
 */
#include "pbkdf2_sha256.h"

#include <cstring>

namespace hal {

namespace {

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

const uint32_t IV256[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

/* Bit length of a message that is one 64-byte HMAC pad block followed by a
 * 32-byte digest: the trailer of every fixed-shape block below. */
const uint32_t HMAC_32B_MSG_BITS = (64 + 32) * 8;

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

inline void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* One SHA-256 compression of `blk` (16 big-endian-decoded words) into `st`.
 * `W` is caller-owned scratch so the caller can wipe it once at the end rather
 * than per compression. Data-independent control flow throughout. */
void sha256_compress(uint32_t st[8], const uint32_t blk[16], uint32_t W[64])
{
    for (int i = 0; i < 16; i++) W[i] = blk[i];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(W[i - 15], 7) ^ rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
        uint32_t s1 = rotr(W[i - 2], 17) ^ rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + s0 + W[i - 7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + W[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

/* Minimal streaming SHA-256, used only for the variable-length parts: the
 * passphrase pre-hash (> 64 B) and the first inner HMAC over salt || INT(i).
 * Everything secret lives inside the struct; wipe it when done. */
struct Sha256Stream {
    uint32_t st[8];
    uint8_t buf[64];
    size_t n;
    uint64_t total;
    uint32_t blk[16];
    uint32_t W[64];
};

void stream_init(Sha256Stream *s, const uint32_t st[8], uint64_t already_absorbed)
{
    memcpy(s->st, st, sizeof(s->st));
    s->n = 0;
    s->total = already_absorbed;
}

void stream_block(Sha256Stream *s, const uint8_t *p)
{
    for (int i = 0; i < 16; i++) s->blk[i] = load_be32(p + 4 * i);
    sha256_compress(s->st, s->blk, s->W);
}

void stream_update(Sha256Stream *s, const uint8_t *p, size_t len)
{
    s->total += len;
    while (len > 0) {
        size_t take = 64 - s->n;
        if (take > len) take = len;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        len -= take;
        if (s->n == 64) {
            stream_block(s, s->buf);
            s->n = 0;
        }
    }
}

void stream_final(Sha256Stream *s, uint32_t out[8])
{
    uint64_t bits = s->total * 8;
    s->buf[s->n++] = 0x80;
    if (s->n > 56) {
        memset(s->buf + s->n, 0, 64 - s->n);
        stream_block(s, s->buf);
        s->n = 0;
    }
    memset(s->buf + s->n, 0, 56 - s->n);
    store_be32(s->buf + 56, (uint32_t)(bits >> 32));
    store_be32(s->buf + 60, (uint32_t)bits);
    stream_block(s, s->buf);
    memcpy(out, s->st, 8 * sizeof(uint32_t));
}

}  // namespace

void secure_wipe(void *buf, size_t n)
{
    /* Calling memset through a volatile function pointer stops the compiler
     * from proving the store dead and dropping it. */
    static void *(*const volatile memset_v)(void *, int, size_t) = memset;
    memset_v(buf, 0, n);
}

void pbkdf2_sha256_init(Pbkdf2Sha256Ctx *ctx, const uint8_t *pw, size_t pw_len)
{
    uint8_t key[64];
    uint32_t blk[16];
    uint32_t W[64];
    memset(key, 0, sizeof(key));

    if (pw_len > 64) {
        /* HMAC: keys longer than the block size are replaced by H(K). */
        Sha256Stream s;
        uint32_t d[8];
        stream_init(&s, IV256, 0);
        stream_update(&s, pw, pw_len);
        stream_final(&s, d);
        for (int i = 0; i < 8; i++) store_be32(key + 4 * i, d[i]);
        secure_wipe(&s, sizeof(s));
        secure_wipe(d, sizeof(d));
    } else if (pw_len > 0) {
        memcpy(key, pw, pw_len);
    }

    for (int i = 0; i < 16; i++) blk[i] = load_be32(key + 4 * i) ^ 0x36363636u;
    memcpy(ctx->istate, IV256, sizeof(ctx->istate));
    sha256_compress(ctx->istate, blk, W);

    for (int i = 0; i < 16; i++) blk[i] = load_be32(key + 4 * i) ^ 0x5c5c5c5cu;
    memcpy(ctx->ostate, IV256, sizeof(ctx->ostate));
    sha256_compress(ctx->ostate, blk, W);

    secure_wipe(key, sizeof(key));
    secure_wipe(blk, sizeof(blk));
    secure_wipe(W, sizeof(W));
}

void pbkdf2_sha256_block(const Pbkdf2Sha256Ctx *ctx,
                         const uint8_t *salt, size_t salt_len,
                         uint32_t iterations, uint32_t block_index,
                         uint8_t out[32])
{
    uint32_t W[64];
    uint32_t ib[16];   /* inner block: U_{j-1} || pad, after the ipad block */
    uint32_t ob[16];   /* outer block: inner digest || pad, after the opad block */
    uint32_t s[8];     /* running chaining state */
    uint32_t u[8];     /* U_j */
    uint32_t t[8];     /* T = XOR of all U_j */
    uint8_t idx[4];

    /* The pad tails of both fixed-shape blocks never change. */
    ib[8] = 0x80000000u;
    ob[8] = 0x80000000u;
    for (int i = 9; i < 15; i++) { ib[i] = 0; ob[i] = 0; }
    ib[15] = HMAC_32B_MSG_BITS;
    ob[15] = HMAC_32B_MSG_BITS;

    /* U_1 = HMAC(P, S || INT(i)): the inner hash has a variable-length
     * message, so it goes through the streaming path. */
    {
        Sha256Stream st;
        store_be32(idx, block_index);
        stream_init(&st, ctx->istate, 64);
        stream_update(&st, salt, salt_len);
        stream_update(&st, idx, sizeof(idx));
        stream_final(&st, s);
        secure_wipe(&st, sizeof(st));
    }
    for (int i = 0; i < 8; i++) ob[i] = s[i];
    memcpy(s, ctx->ostate, sizeof(s));
    sha256_compress(s, ob, W);
    for (int i = 0; i < 8; i++) { u[i] = s[i]; t[i] = s[i]; }

    /* U_j = HMAC(P, U_{j-1}): exactly two compressions per iteration. */
    for (uint32_t j = 1; j < iterations; j++) {
        for (int i = 0; i < 8; i++) ib[i] = u[i];
        memcpy(s, ctx->istate, sizeof(s));
        sha256_compress(s, ib, W);
        for (int i = 0; i < 8; i++) ob[i] = s[i];
        memcpy(s, ctx->ostate, sizeof(s));
        sha256_compress(s, ob, W);
        for (int i = 0; i < 8; i++) { u[i] = s[i]; t[i] ^= s[i]; }
    }

    for (int i = 0; i < 8; i++) store_be32(out + 4 * i, t[i]);

    secure_wipe(W, sizeof(W));
    secure_wipe(ib, sizeof(ib));
    secure_wipe(ob, sizeof(ob));
    secure_wipe(s, sizeof(s));
    secure_wipe(u, sizeof(u));
    secure_wipe(t, sizeof(t));
}

bool pbkdf2_hmac_sha256(const uint8_t *pw, size_t pw_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations,
                        uint8_t *out, size_t out_len)
{
    if (iterations == 0 || out_len == 0) return false;
    if ((out_len - 1) / 32 >= 0xFFFFFFFFu) return false;

    Pbkdf2Sha256Ctx ctx;
    uint8_t blk[32];
    pbkdf2_sha256_init(&ctx, pw, pw_len);

    uint32_t index = 1;
    size_t off = 0;
    while (off < out_len) {
        pbkdf2_sha256_block(&ctx, salt, salt_len, iterations, index++, blk);
        size_t take = out_len - off;
        if (take > sizeof(blk)) take = sizeof(blk);
        memcpy(out + off, blk, take);
        off += take;
    }

    secure_wipe(&ctx, sizeof(ctx));
    secure_wipe(blk, sizeof(blk));
    return true;
}

}  // namespace hal
