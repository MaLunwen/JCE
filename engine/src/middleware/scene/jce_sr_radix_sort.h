/*
 * jce_sr_radix_sort.h - LSD radix sort of an index array by a parallel u32 key.
 *
 * WHY THIS EXISTS
 * The scene renderer orders its primitive instance batch front-to-back before
 * submitting. A comparison sort did that with an indirect comparator call per
 * comparison, each chasing two random offsets into a multi-megabyte instance
 * array: at 26,656 instances it measured 2.73 ms per frame - 89% of the whole
 * flush phase and ~20% of the frame, on a workload that ends up issuing 147
 * draw calls. This is O(n) with no indirect call and sequential writes.
 *
 * FLOAT KEYS
 * Callers sorting by a NON-NEGATIVE float (a squared distance, say) can pass
 * its raw IEEE-754 bit pattern as the key with no transformation: for x >= 0
 * the bit pattern orders identically to the float. This is NOT true once
 * negatives are involved - they compare inverted - so a caller with signed
 * keys must flip them first (see jce_radix_key_from_float, which handles both
 * and is what any new caller should use).
 *
 * STABILITY
 * Stable, unlike qsort: equal keys keep their input order rather than an
 * arbitrary one. That makes frame-to-frame submit order reproducible for
 * equal-depth instances, which a comparison sort did not guarantee.
 */
#ifndef JCE_SR_RADIX_SORT_H
#define JCE_SR_RADIX_SORT_H

#include <stdint.h>
#include <string.h>

/* Order-preserving u32 key for ANY float, including negatives and -0.0.
 * Non-negative: set the sign bit. Negative: invert every bit. */
static inline uint32_t jce_radix_key_from_float(float f)
{
    union { float f; uint32_t u; } c;
    c.f = f;
    return (c.u & 0x80000000u) ? ~c.u : (c.u | 0x80000000u);
}

/*
 * Sorts `idx` (n entries) so that the parallel `keys` are non-decreasing.
 * keys[i] is the key of idx[i] - the two arrays move together.
 *
 * `scratch` must hold at least 2*n uint32_t (key_tmp, idx_tmp). `keys` is
 * permuted in place alongside `idx`; callers that need the original key order
 * must keep their own copy.
 *
 * Passes whose byte is uniform across all keys are skipped, which removes the
 * high bytes for typical distance keys. Because the executed pass count can be
 * odd, the result is copied back explicitly rather than relying on parity.
 */
static inline void jce_radix_sort_u32(uint32_t *keys, uint32_t *idx,
                                      uint32_t n, uint32_t *scratch)
{
    if (!keys || !idx || !scratch || n < 2u) return;

    uint32_t *key_cur = keys;
    uint32_t *key_alt = scratch;
    uint32_t *idx_cur = idx;
    uint32_t *idx_alt = scratch + n;

    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t count[256];
        memset(count, 0, sizeof count);
        for (uint32_t i = 0; i < n; i++) count[(key_cur[i] >> shift) & 0xFFu]++;
        if (count[(key_cur[0] >> shift) & 0xFFu] == n) continue;  /* uniform byte */

        uint32_t sum = 0;
        for (int b = 0; b < 256; b++) {
            const uint32_t c = count[b];
            count[b] = sum;
            sum += c;
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t k = key_cur[i];
            const uint32_t d = count[(k >> shift) & 0xFFu]++;
            key_alt[d] = k;
            idx_alt[d] = idx_cur[i];
        }
        { uint32_t *t = key_cur; key_cur = key_alt; key_alt = t; }
        { uint32_t *t = idx_cur; idx_cur = idx_alt; idx_alt = t; }
    }

    if (idx_cur != idx) {
        memcpy(idx,  idx_cur, (size_t)n * sizeof(uint32_t));
        memcpy(keys, key_cur, (size_t)n * sizeof(uint32_t));
    }
}

#endif /* JCE_SR_RADIX_SORT_H */
