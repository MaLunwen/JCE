/*
 * jce_hash.h  Shared non-cryptographic hash helpers (FNV-1a).
 *
 * Cheap keying for in-session caches, epochs and registries (string or
 * small-struct keys).  NOT collision-resistant: use the xxhash dependency
 * for content hashing of large buffers, and keep any hash that is
 * persisted to disk (cache files, wire formats) pinned to its own
 * implementation so the on-disk key never silently changes.
 */

#ifndef JCE_HASH_H
#define JCE_HASH_H

#include <jce/os/core/jce_defs.h>

#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* FNV-1a offset bases and primes (public-domain constants). */
#define JCE_FNV1A32_INIT  2166136261u
#define JCE_FNV1A32_PRIME 16777619u
#define JCE_FNV1A64_INIT  1469598103934665603ULL
#define JCE_FNV1A64_PRIME 1099511628211ULL

/* Folds `len` bytes into an existing 32-bit FNV-1a state `h`.  Seed with
 * JCE_FNV1A32_INIT for a fresh key, or chain calls to mix several fields
 * into one key. */
JCE_INLINE uint32_t jce_fnv1a32_append(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= JCE_FNV1A32_PRIME; }
    return h;
}

JCE_INLINE uint32_t jce_fnv1a32(const void *data, size_t len)
{
    return jce_fnv1a32_append(JCE_FNV1A32_INIT, data, len);
}

JCE_INLINE uint32_t jce_fnv1a32_str(const char *s)
{
    uint32_t h = JCE_FNV1A32_INIT;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= JCE_FNV1A32_PRIME; }
    return h;
}

/* 64-bit variants (byte-wise, standard FNV-1a). */
JCE_INLINE uint64_t jce_fnv1a64_append(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= JCE_FNV1A64_PRIME; }
    return h;
}

JCE_INLINE uint64_t jce_fnv1a64(const void *data, size_t len)
{
    return jce_fnv1a64_append(JCE_FNV1A64_INIT, data, len);
}

JCE_INLINE uint64_t jce_fnv1a64_str(const char *s)
{
    uint64_t h = JCE_FNV1A64_INIT;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= JCE_FNV1A64_PRIME; }
    return h;
}

JCE_EXTERN_C_END

#endif /* JCE_HASH_H */
