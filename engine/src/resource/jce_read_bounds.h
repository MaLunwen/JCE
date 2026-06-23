/*
 * jce_read_bounds.h  Overflow-safe bounds primitives for untrusted blobs.
 *
 * Cooked containers (.pak / .jceasset / bundle) carry file-controlled
 * offsets, sizes and counts.  Validating them with naive arithmetic
 * (`offset + size <= limit`, `count * elem <= avail`) wraps around in
 * uint64 / size_t, so a crafted near-maximum value passes the check and the
 * reader then dereferences out of bounds.  Every region/count check in the
 * resource readers must go through these helpers instead.
 *
 * Header-only (static inline) so callers in any resource TU share one
 * definition without a link dependency.
 */

#ifndef JCE_READ_BOUNDS_H
#define JCE_READ_BOUNDS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True iff the byte range [off, off + len) lies entirely within a buffer of
 * `limit` bytes.  Overflow-safe: never computes off + len (which can wrap).
 */
static inline bool jce_region_in_bounds(uint64_t off, uint64_t len,
                                        uint64_t limit)
{
    return off <= limit && len <= limit - off;
}

/*
 * True iff `count` elements of `elem` bytes each fit within `avail` bytes.
 * Overflow-safe: never computes count * elem (which can wrap).  A zero
 * element size trivially fits (no bytes consumed).
 */
static inline bool jce_count_fits(uint64_t count, uint64_t elem,
                                  uint64_t avail)
{
    return elem == 0 || count <= avail / elem;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_READ_BOUNDS_H */
