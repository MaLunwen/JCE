/*
 * jce_shadow_key.h -- the CSM shadow-cache caster key.
 *
 * Split out of jce_sr_internal.h for one reason: that header pulls in bgfx,
 * and this is pure integer arithmetic.  Keeping it separate is what lets the
 * key be tested headlessly -- and every property it has to satisfy is one that
 * no rendered frame would reveal.
 *
 * Layer: Scene (L4) -- PRIVATE.
 */

#ifndef JCE_SHADOW_KEY_H
#define JCE_SHADOW_KEY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Shadow caster key ──────────────────────────────────────────────────
 *
 * The CSM cache is reused whenever the set of shadow casters and their
 * transforms are unchanged, and this key is how "unchanged" is decided.  It
 * has to be order-independent, because the caster walk is chunked across
 * worker threads and the chunks complete in whatever order they finish.
 *
 * It also has to NOT self-cancel, and that is why the combiner is wrapping
 * ADDITION rather than XOR.  With XOR, folding the same caster twice in one
 * frame cancels it out entirely: the key comes back as if that object were not
 * a caster at all, so moving it stops invalidating the cache and its shadow
 * freezes in place.  Nothing about that failure is visible in the key -- it is
 * a perfectly ordinary-looking number.
 *
 * Addition keeps commutativity (so chunk order still does not matter) while a
 * double fold doubles the contribution instead of erasing it. */
static inline uint64_t sr_shadow_mix64(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31; return x;
}

/* Fold one caster (entity id + transform generation) into `key`. */
static inline uint64_t sr_shadow_fold_caster(uint64_t key, uint64_t entity,
                                             uint64_t gen)
{
    return key + sr_shadow_mix64(entity * 1099511628211ull + gen);
}

/* Combine an independently-accumulated partial key (one worker chunk) into the
 * running key.  Must use the SAME combiner as the per-caster fold, or the two
 * halves of the accumulation would disagree about what "unchanged" means. */
static inline uint64_t sr_shadow_fold_part(uint64_t key, uint64_t part)
{
    return key + part;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADOW_KEY_H */
