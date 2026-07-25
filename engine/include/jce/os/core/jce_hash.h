/*
 * jce_hash.h  Shared non-cryptographic hash helpers (FNV-1a byte hashes +
 * integer finalizers).
 *
 * Cheap keying for in-session caches, epochs and registries (string or
 * small-struct keys).  NOT collision-resistant: use the xxhash dependency
 * for content hashing of large buffers, and keep any hash that is
 * persisted to disk (cache files, wire formats) pinned to its own
 * implementation so the on-disk key never silently changes.
 *
 * The integer finalizers below are the exact, published bit-mixers — the
 * constants and shift amounts are part of the contract.  Never "tidy" them:
 * a single changed digit changes every bucket index in every caller.
 */

#ifndef JCE_HASH_H
#define JCE_HASH_H

#include <jce/os/core/jce_defs.h>

#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* FNV-1a offset bases and primes (public-domain constants).
 *
 * The 64-bit basis read 1469598103934665603 until 2026-07-25 — the canonical
 * value with its trailing digit dropped, so this was never really FNV-1a and
 * would not agree with any external implementation.  Corrected after proving
 * no caller persists the result: all seven users
 * (jce_scene_particles.c, jce_scene_sequencer.c, jce_scene_video.c) compute an
 * in-session "has the authoring changed?" epoch and compare it against a value
 * computed by the same process; the scene parser explicitly zeroes
 * asset_epoch on load, and jce_fnv1a64_* appears nowhere in save/, resource/,
 * application/ or tools/.  Nothing on disk, in a PAK, or in an asset id moves.
 *
 * Note this is a DIFFERENT constant from JCE_HASH_DET64_OFFSET below, which is
 * pinned to the deterministic scene-compiler / AI-director hashes.  They are
 * now numerically equal but must stay separately named: this one is a general
 * in-session helper, that one is a frozen wire/replay contract. */
#define JCE_FNV1A32_INIT  2166136261u
#define JCE_FNV1A32_PRIME 16777619u
#define JCE_FNV1A64_INIT  14695981039346656037ULL
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

/* ── Pinned deterministic FNV-1a-64 ───────────────────────────────────
 * The scene compiler (stable entity ids, per-role RNG seeds, the
 * FrozenPlan hash) and the AI scene director (cache key) are all built
 * from this exact set.  Those values are compared across builds and
 * machines and travel in the FrozenPlan wire format, so the offset, the
 * prime, the shift amounts AND the little-endian byte order below are
 * part of the on-disk contract — changing any of them silently
 * invalidates every plan and cache entry that already exists.
 *
 * This uses the CANONICAL FNV-1a-64 offset basis, which is deliberately
 * NOT the JCE_FNV1A64_INIT above: that one is a long-standing truncated
 * value which its own (in-session, non-persisted) callers are pinned to.
 * The two must never be merged.
 */
#define JCE_HASH_DET64_OFFSET 14695981039346656037ULL
#define JCE_HASH_DET64_PRIME  1099511628211ULL

JCE_INLINE uint64_t jce_hash_det64_bytes(uint64_t hash, const void *data,
                                         size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;

    for (i = 0u; i < size; ++i) {
        hash ^= (uint64_t)bytes[i];
        hash *= JCE_HASH_DET64_PRIME;
    }
    return hash;
}

/* Integer fields are folded in through their little-endian byte image so
 * the resulting key is identical on big- and little-endian hosts. */
JCE_INLINE uint64_t jce_hash_det64_u16(uint64_t hash, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8u) & 0xffu);
    return jce_hash_det64_bytes(hash, bytes, sizeof(bytes));
}

JCE_INLINE uint64_t jce_hash_det64_u32(uint64_t hash, uint32_t value)
{
    uint8_t bytes[4];
    uint32_t i;

    for (i = 0u; i < 4u; ++i)
        bytes[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    return jce_hash_det64_bytes(hash, bytes, sizeof(bytes));
}

JCE_INLINE uint64_t jce_hash_det64_u64(uint64_t hash, uint64_t value)
{
    uint8_t bytes[8];
    uint32_t i;

    for (i = 0u; i < 8u; ++i)
        bytes[i] = (uint8_t)((value >> (i * 8u)) & 0xffu);
    return jce_hash_det64_bytes(hash, bytes, sizeof(bytes));
}

/* ── Integer finalizers ───────────────────────────────────────────────
 * Avalanche an already-dense integer key (entity id, voice id, node id) so
 * the low bits are usable as an open-addressing bucket index.  These are
 * bit-mixers, not byte hashes: feed them an integer, mask the result.
 */

/* MurmurHash3 64-bit finalizer (fmix64).  The default for 64-bit ids. */
JCE_INLINE uint64_t jce_hash_fmix64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* SplitMix64 finalizer (Stafford mix13).  Slightly better avalanche than
 * fmix64; kept separate because the two produce different values and some
 * callers are pinned to one of them. */
JCE_INLINE uint64_t jce_hash_splitmix64(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* 32-bit integer finalizer ("lowbias32", Wellons).  Note this is NOT
 * MurmurHash3's fmix32 — different constants, different values. */
JCE_INLINE uint32_t jce_hash_mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

JCE_EXTERN_C_END

#endif /* JCE_HASH_H */
