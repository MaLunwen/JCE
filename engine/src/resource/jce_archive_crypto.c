/*
 * jce_archive_crypto.c  ChaCha20 (RFC 8439 §2.3-2.4) stream cipher.
 *
 * Self-contained, dependency-free implementation used for the archive's
 * optional payload encryption (spec §9.2).  Little-endian throughout, matching
 * the archive's on-disk byte order, so encrypted bytes round-trip identically
 * across platforms.
 */

#include "resource/jce_archive_crypto.h"
#include "os/core/jce_sha256_internal.h"

#include <string.h>

void jce_archive_crypto_zero(void *ptr, size_t size) {
    jce_secure_zero_internal(ptr, size);
}

static void hmac_sha256_parts(const uint8_t *key, size_t key_len,
                              const uint8_t *first, size_t first_len,
                              const uint8_t *second, size_t second_len,
                              uint8_t out[32]) {
    uint8_t key_block[64] = {0};
    uint8_t inner_pad[64];
    uint8_t outer_pad[64];
    uint8_t inner_hash[32];
    JceSha256Internal ctx;

    if (key_len > sizeof(key_block)) {
        jce_sha256_init_internal(&ctx);
        jce_sha256_update_internal(&ctx, key, key_len);
        jce_sha256_final_internal(&ctx, key_block);
    } else if (key_len > 0) {
        memcpy(key_block, key, key_len);
    }
    for (size_t i = 0; i < sizeof(key_block); ++i) {
        inner_pad[i] = (uint8_t)(key_block[i] ^ 0x36u);
        outer_pad[i] = (uint8_t)(key_block[i] ^ 0x5cu);
    }

    jce_sha256_init_internal(&ctx);
    jce_sha256_update_internal(&ctx, inner_pad, sizeof(inner_pad));
    jce_sha256_update_internal(&ctx, first, first_len);
    jce_sha256_update_internal(&ctx, second, second_len);
    jce_sha256_final_internal(&ctx, inner_hash);

    jce_sha256_init_internal(&ctx);
    jce_sha256_update_internal(&ctx, outer_pad, sizeof(outer_pad));
    jce_sha256_update_internal(&ctx, inner_hash, sizeof(inner_hash));
    jce_sha256_final_internal(&ctx, out);

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
