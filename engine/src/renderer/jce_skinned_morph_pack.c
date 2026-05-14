/*
 * jce_skinned_morph_pack.c  Top-K weight selector.
 *
 * Partial-selection sort (descending magnitude) keeps the top-K
 * weights without full-array sort.  For typical N <= 32 and K <= 8
 * this is faster than qsort + memory bandwidth on small arrays.
 */

#include <jce/renderer/jce_skinned_morph_pack.h>

#include <math.h>
#include <string.h>

uint32_t jce_skinned_pack_morph_weights(const JceMorphSet *set,
                                         const float *weights,
                                         float *out_weights,
                                         uint8_t *out_indices,
                                         uint32_t max)
{
    if (!set || !weights || !out_weights || !out_indices || max == 0) return 0;
    uint32_t total = jce_morph_set_target_count(set);

    /* Zero-pad outputs first. */
    for (uint32_t i = 0; i < max; ++i) {
        out_weights[i] = 0.0f;
        out_indices[i] = 0;
    }
    if (total == 0) return 0;

    /* Track which sources have been picked so we don't double-count. */
    uint8_t picked[256] = {0};
    uint32_t picked_cap = total > 256u ? 256u : total;

    uint32_t written = 0;
    while (written < max) {
        int best = -1;
        float best_mag = 0.0f;
        for (uint32_t i = 0; i < picked_cap; ++i) {
            if (picked[i]) continue;
            float m = fabsf(weights[i]);
            if (m > best_mag) {
                best_mag = m;
                best = (int)i;
            }
        }
        if (best < 0 || best_mag <= 0.0f) break;
        out_weights[written] = weights[best];
        out_indices[written] = (uint8_t)best;
        picked[best] = 1;
        written++;
    }
    return written;
}
