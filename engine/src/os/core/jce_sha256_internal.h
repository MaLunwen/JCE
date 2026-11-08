/* Single private SHA-256 owner shared by diagnostics and Archive HMAC. */
#ifndef JCE_SHA256_INTERNAL_H
#define JCE_SHA256_INTERNAL_H
#include <stddef.h>
#include <stdint.h>
typedef struct JceSha256Internal {
    uint32_t state[8];
    uint64_t total_size;
    uint8_t  block[64];
    size_t   block_size;
} JceSha256Internal;
void jce_sha256_init_internal(JceSha256Internal *ctx);
void jce_sha256_update_internal(JceSha256Internal *ctx, const uint8_t *data, size_t size);
void jce_sha256_final_internal(JceSha256Internal *ctx, uint8_t out[32]);
/* Borrowed readable bytes for the complete call; NULL is valid only at size 0. */
void jce_sha256_internal(const void *data, size_t size, uint8_t out[32]);
/* Volatile erasure; Archive's existing private wrapper delegates here. */
void jce_secure_zero_internal(void *ptr, size_t size);
#endif
