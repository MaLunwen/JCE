/*
 * jce_skinned_morph_pack.h  Pack JceMorphSet weights for shader upload.
 *
 * The renderer's skinned-morph shader path expects a flat array of
 * floats holding the active morph weights, plus a tightly-packed
 * index array telling the vertex shader which targets to apply.
 * This helper bridges B6.4's JceMorphSet + a per-instance weights
 * vector into the layout the (future Batch 11) shader will consume.
 *
 * Two outputs are produced:
 *   - `out_weights[max]` — float array of length `max`, filled with
 *     up to `max` non-zero weights from `weights[set->target_count]`;
 *     zero-padded if fewer non-zero values exist.
 *   - `out_indices[max]` — uint8 indices identifying which targets
 *     those weights refer to.
 *
 * The renderer can then `bgfx::setUniform()` the weights vec4 array
 * (max divisible by 4 in practice) and look up vertex deltas by
 * index in the vertex shader.
 *
 * Layer: renderer (Layer 5) — public.
 */

#ifndef JCE_SKINNED_MORPH_PACK_H
#define JCE_SKINNED_MORPH_PACK_H

#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_morph_target.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Sort weights[] in descending magnitude order, take the top `max`,
 * fill `out_weights` + `out_indices`.  Returns the actual count of
 * non-zero weights written (0..max).  Zero-pads the rest. */
JCE_API uint32_t jce_skinned_pack_morph_weights(
    const JceMorphSet *set,
    const float       *weights,        /* set->target_count elements */
    float             *out_weights,    /* max elements */
    uint8_t           *out_indices,    /* max elements */
    uint32_t           max);

JCE_EXTERN_C_END

#endif /* JCE_SKINNED_MORPH_PACK_H */
