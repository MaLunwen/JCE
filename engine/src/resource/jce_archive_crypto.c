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

void jce_archive_derive_nonce(uint64_t path_hash,
                              uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES]) {
    /* Low 64 bits = path hash (unique per entry), high 32 bits = 0. */
    for (int i = 0; i < 8; ++i) nonce[i] = (uint8_t)(path_hash >> (i * 8));
    nonce[8] = nonce[9] = nonce[10] = nonce[11] = 0;
}
