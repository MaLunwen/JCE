/*
 * jce_hashmap.h  Shared arithmetic for hand-rolled open-addressing tables.
 *
 * This is deliberately NOT a container.  The engine has ~20 open-addressed
 * tables (renderer caches, scene side-tables, asset registry, audio voice
 * maps, ...) and each one is intrusive on purpose: the key lives in the same
 * struct as the payload, so a probe is one cache line and a hit needs no
 * second indirection.  A generic map over (void pointer + element size) would
 * put a memcpy (or a key -> index hop) on per-frame paths that must stay flat
 * on the low-end baseline, and C99 has no other way to write one without macro
 * codegen.
 *
 * What IS worth sharing is the arithmetic that every such table repeats and
 * that is easy to get subtly wrong.  Both helpers below are inline and
 * branch-for-branch what the callers already wrote — using them costs
 * nothing at any call site.
 *
 * Every capacity here is a power of two, so the bucket index is
 * `hash & (cap - 1)` (see jce_hash.h for the integer finalizers).
 */

#ifndef JCE_HASHMAP_H
#define JCE_HASHMAP_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Smallest power of two that is >= both `need` and `min_cap`.
 * `min_cap` must itself be a power of two (it is the table's floor capacity).
 * `need` == 0 yields `min_cap`.  The 0x80000000 stop keeps a caller that
 * computes `need` from untrusted arithmetic from spinning forever; no real
 * table gets within three orders of magnitude of it. */
JCE_INLINE uint32_t jce_hashmap_cap_pow2(uint32_t need, uint32_t min_cap)
{
    uint32_t cap = min_cap ? min_cap : 1u;
    while (cap < need && cap < 0x80000000u) cap <<= 1;
    return cap;
}

/* Canonical Knuth backward-shift-delete predicate (Algorithm R).
 *
 * After emptying `hole`, walk `probe` forward to the end of the cluster; this
 * returns true when the entry sitting at `probe` must slide back into `hole`
 * to stay reachable — that is, when its ideal slot `home` is NOT cyclically
 * inside the open interval (hole, probe].  When `home` IS in that range the
 * entry already sits on/after the hole along its own probe path and must be
 * left where it is.
 *
 * `cap` must be a power of two.  Note the comparison is `>=`, not `>`: an
 * entry whose home IS the hole (the ordinary two-keys-one-bucket collision)
 * becomes unreachable the moment the hole is left empty, so it must move. */
JCE_INLINE bool jce_hashmap_shift_back(uint32_t home, uint32_t hole,
                                       uint32_t probe, uint32_t cap)
{
    const uint32_t mask = cap - 1u;
    return ((probe - home) & mask) >= ((probe - hole) & mask);
}

JCE_EXTERN_C_END

#endif /* JCE_HASHMAP_H */
