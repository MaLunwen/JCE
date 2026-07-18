/*
 * jce_archive_crypto.c  ChaCha20 (RFC 8439 §2.3-2.4) stream cipher.
 *
 * Self-contained, dependency-free implementation used for the archive's
 * optional payload encryption (spec §9.2).  Little-endian throughout, matching
 * the archive's on-disk byte order, so encrypted bytes round-trip identically
 * across platforms.
 */

#include "resource/jce_archive_crypto.h"

#include <string.h>

typedef struct JceArchiveSha256 {
    uint32_t state[8];
    uint64_t total_size;
    uint8_t  block[64];
    size_t   block_size;
} JceArchiveSha256;

static const uint32_t k_sha256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
};

static uint32_t rotr32(uint32_t v, int n) {
    return (v >> n) | (v << (32 - n));
}

static uint32_t rd_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void jce_archive_crypto_zero(void *ptr, size_t size) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (size-- > 0) *p++ = 0;
}

static void sha256_transform(JceArchiveSha256 *ctx, const uint8_t block[64]) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; ++i) w[i] = rd_be32(block + (size_t)i * 4u);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
    d = ctx->state[3]; e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k_sha256[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
    jce_archive_crypto_zero(w, sizeof(w));
}

static void sha256_init(JceArchiveSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u,
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->total_size = 0;
    ctx->block_size = 0;
}

static void sha256_update(JceArchiveSha256 *ctx, const uint8_t *data,
                          size_t size) {
    if (!ctx || (!data && size != 0)) return;
    ctx->total_size += size;
    while (size > 0) {
        size_t available = sizeof(ctx->block) - ctx->block_size;
        size_t take = size < available ? size : available;
        memcpy(ctx->block + ctx->block_size, data, take);
        ctx->block_size += take;
        data += take;
        size -= take;
        if (ctx->block_size == sizeof(ctx->block)) {
            sha256_transform(ctx, ctx->block);
            ctx->block_size = 0;
        }
    }
}

static void sha256_final(JceArchiveSha256 *ctx, uint8_t out[32]) {
    uint64_t bit_size = ctx->total_size * 8u;
    ctx->block[ctx->block_size++] = 0x80u;
    if (ctx->block_size > 56u) {
        memset(ctx->block + ctx->block_size, 0,
               sizeof(ctx->block) - ctx->block_size);
        sha256_transform(ctx, ctx->block);
        ctx->block_size = 0;
    }
    memset(ctx->block + ctx->block_size, 0, 56u - ctx->block_size);
    for (int i = 0; i < 8; ++i)
        ctx->block[63 - i] = (uint8_t)(bit_size >> (i * 8));
    sha256_transform(ctx, ctx->block);
    for (int i = 0; i < 8; ++i) wr_be32(out + (size_t)i * 4u,
                                        ctx->state[i]);
    jce_archive_crypto_zero(ctx, sizeof(*ctx));
}

static void hmac_sha256_parts(const uint8_t *key, size_t key_len,
                              const uint8_t *first, size_t first_len,
                              const uint8_t *second, size_t second_len,
                              uint8_t out[32]) {
    uint8_t key_block[64] = {0};
    uint8_t inner_pad[64];
    uint8_t outer_pad[64];
    uint8_t inner_hash[32];
    JceArchiveSha256 ctx;

    if (key_len > sizeof(key_block)) {
        sha256_init(&ctx);
        sha256_update(&ctx, key, key_len);
        sha256_final(&ctx, key_block);
    } else if (key_len > 0) {
        memcpy(key_block, key, key_len);
    }
    for (size_t i = 0; i < sizeof(key_block); ++i) {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36u);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5cu);
    }

    sha256_init(&ctx);
    sha256_update(&ctx, inner_pad, sizeof(inner_pad));
    sha256_update(&ctx, first, first_len);
    sha256_update(&ctx, second, second_len);
    sha256_final(&ctx, inner_hash);

    sha256_init(&ctx);
    sha256_update(&ctx, outer_pad, sizeof(outer_pad));
    sha256_update(&ctx, inner_hash, sizeof(inner_hash));
    sha256_final(&ctx, out);

    jce_archive_crypto_zero(key_block, sizeof(key_block));
    jce_archive_crypto_zero(inner_pad, sizeof(inner_pad));
    jce_archive_crypto_zero(outer_pad, sizeof(outer_pad));
    jce_archive_crypto_zero(inner_hash, sizeof(inner_hash));
}

void jce_archive_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t out[JCE_ARCHIVE_AUTH_BYTES]) {
    if (!out || (!key && key_len != 0) || (!data && data_len != 0)) return;
    hmac_sha256_parts(key, key_len, data, data_len, NULL, 0, out);
}

static void derive_one(const uint8_t master[32], const char *label,
                       uint32_t salt32, uint8_t out[32]) {
    uint8_t salt[4];
    salt[0] = (uint8_t)salt32;
    salt[1] = (uint8_t)(salt32 >> 8);
    salt[2] = (uint8_t)(salt32 >> 16);
    salt[3] = (uint8_t)(salt32 >> 24);
    hmac_sha256_parts(master, 32, (const uint8_t *)label, strlen(label),
                      salt, sizeof(salt), out);
}

void jce_archive_secure_keys_derive(const uint8_t master[32], uint32_t salt32,
                                    JceArchiveSecureKeys *out) {
    if (!master || !out) return;
    derive_one(master, "JCE-PATH-v1",  salt32, out->path);
    derive_one(master, "JCE-ENC-v1",   salt32, out->enc);
    derive_one(master, "JCE-NONCE-v1", salt32, out->nonce);
    derive_one(master, "JCE-AUTH-v1",  salt32, out->auth);
}

uint64_t jce_archive_secure_path_hash(const JceArchiveSecureKeys *keys,
                                      const char *normalized_path,
                                      size_t path_len) {
    uint8_t digest[32];
    uint64_t value;
    if (!keys || !normalized_path || path_len == 0) return 0;
    jce_archive_hmac_sha256(keys->path, sizeof(keys->path),
                            (const uint8_t *)normalized_path, path_len,
                            digest);
    value = (uint64_t)digest[0] | (uint64_t)digest[1] << 8 |
            (uint64_t)digest[2] << 16 | (uint64_t)digest[3] << 24 |
            (uint64_t)digest[4] << 32 | (uint64_t)digest[5] << 40 |
            (uint64_t)digest[6] << 48 | (uint64_t)digest[7] << 56;
    jce_archive_crypto_zero(digest, sizeof(digest));
    return value ? value : UINT64_C(1);
}

void jce_archive_secure_nonce(const JceArchiveSecureKeys *keys,
                              uint8_t compression, uint16_t dict_id,
                              const uint8_t *stored_plaintext,
                              size_t stored_size, uint8_t nonce[12]) {
    uint8_t meta[11];
    uint8_t digest[32];
    if (!keys || !nonce || (!stored_plaintext && stored_size != 0)) return;
    meta[0] = compression;
    meta[1] = (uint8_t)dict_id;
    meta[2] = (uint8_t)(dict_id >> 8);
    for (int i = 0; i < 8; ++i)
        meta[3 + i] = (uint8_t)((uint64_t)stored_size >> (i * 8));
    hmac_sha256_parts(keys->nonce, sizeof(keys->nonce), meta, sizeof(meta),
                      stored_plaintext, stored_size, digest);
    memcpy(nonce, digest, JCE_ARCHIVE_NONCE_BYTES);
    jce_archive_crypto_zero(digest, sizeof(digest));
}

int jce_archive_crypto_equal(const uint8_t *a, const uint8_t *b, size_t size) {
    uint8_t diff = 0;
    if ((!a || !b) && size != 0) return 0;
    for (size_t i = 0; i < size; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rotl32(uint32_t v, int c) {
    return (v << c) | (v >> (32 - c));
}

#define QR(a, b, c, d)                       \
    a += b; d ^= a; d = rotl32(d, 16);       \
    c += d; b ^= c; b = rotl32(b, 12);       \
    a += b; d ^= a; d = rotl32(d, 8);        \
    c += d; b ^= c; b = rotl32(b, 7)

static void chacha20_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; ++i) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; ++i) {
        uint32_t v = x[i] + in[i];
        out[i * 4 + 0] = (uint8_t)(v);
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

void jce_archive_chacha20_xor(const uint8_t key[JCE_ARCHIVE_KEY_BYTES],
                              const uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES],
                              uint32_t counter,
                              const uint8_t *in, uint8_t *out, size_t len) {
    uint32_t state[16];
    state[0] = 0x61707865u; /* "expand 32-byte k" */
    state[1] = 0x3320646eu;
    state[2] = 0x79622d32u;
    state[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) state[4 + i] = rd_le32(key + i * 4);
    state[12] = counter;
    state[13] = rd_le32(nonce + 0);
    state[14] = rd_le32(nonce + 4);
    state[15] = rd_le32(nonce + 8);

    uint8_t block[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(state, block);
        size_t n = len - off;
        if (n > 64) n = 64;
        for (size_t i = 0; i < n; ++i) out[off + i] = in[off + i] ^ block[i];
        off += n;
        state[12]++; /* next block counter (LE word) */
    }
}

void jce_archive_derive_nonce(uint64_t path_hash, uint32_t salt32,
                              uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES]) {
    /* Low 64 bits = path hash (unique per entry), high 32 bits = the
     * per-archive salt (header nonce_salt32; 0 for legacy archives). */
    for (int i = 0; i < 8; ++i) nonce[i] = (uint8_t)(path_hash >> (i * 8));
    for (int i = 0; i < 4; ++i) nonce[8 + i] = (uint8_t)(salt32 >> (i * 8));
}
