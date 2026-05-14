/*
 * jce_taa_jitter.h  Halton sequence + sub-pixel jitter math for TAA.
 *
 * Pure math helpers that the TAA path uses to derive a deterministic,
 * low-discrepancy jitter offset per frame.  Decoupling them from
 * `jce_taa.c` (which couples to bgfx) means we can unit-test the
 * sequence independently and reuse it for SSAO supersampling,
 * stochastic effects, etc.
 *
 * The Halton sequence with bases (2, 3) is the standard choice for
 * TAA: well-distributed in the unit square, deterministic, and
 * cheap to compute (no LUT).  Each frame's offset is in [-0.5, 0.5]
 * sub-pixels — the renderer adds it to the projection matrix's
 * translation column so the rasteriser samples slightly different
 * sub-pixels each frame.
 *
 * Layer: renderer (Layer 5) — public.
 */

#ifndef JCE_TAA_JITTER_H
#define JCE_TAA_JITTER_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Standard van der Corput / Halton sequence value at index `i` with
 * base `b`.  Returns a value in [0, 1).  `b` should be a small prime
 * (2, 3, 5, 7).  `i` 0 returns 0; subsequent indices fill the [0, 1)
 * interval with low discrepancy. */
JCE_API float jce_taa_halton(uint32_t i, uint32_t b);

/* Compute the sub-pixel jitter offset for `frame_index` at the given
 * RT size.  `*out_x` and `*out_y` are in NDC units — equivalent to
 * pixel_offset / width_or_height — and lie in [-0.5/w, +0.5/w] and
 * [-0.5/h, +0.5/h] respectively.  Use `sequence_length` 8 or 16 for
 * standard TAA; the function wraps frame_index modulo it. */
JCE_API void  jce_taa_jitter_offset(uint32_t frame_index,
                                    uint32_t sequence_length,
                                    uint32_t rt_width,
                                    uint32_t rt_height,
                                    float   *out_ndc_x,
                                    float   *out_ndc_y);

/* Apply a jitter offset to a column-major projection matrix in place.
 * Shifts the translation row/column so the resulting frustum samples
 * the same world points as if the camera had moved by `ndc_x/y` in
 * screen space.  Standard TAA technique — Unity's BuildJitterMatrix. */
JCE_API void  jce_taa_jitter_projection(jce_mat4 *proj,
                                         float ndc_x, float ndc_y);

/* Inverse: undo a jitter applied to a projection matrix (subtracts
 * the offset).  Useful when chaining post-jittered passes that need
 * the un-jittered transform. */
JCE_API void  jce_taa_unjitter_projection(jce_mat4 *proj,
                                           float ndc_x, float ndc_y);

JCE_EXTERN_C_END

#endif /* JCE_TAA_JITTER_H */
