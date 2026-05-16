/*
 * jce_skinned_morph_bind.h  Build vec4-packed morph uniform descriptor.
 *
 * Sits on top of `jce_skinned_morph_pack` — consumes the top-N weights
 * + indices and lays them out as the four-channel uniform arrays the
 * vertex shader expects:
 *
 *   - `weights_vec4` :  ceil(max / 4) vec4 entries, weights[0..max-1]
 *                       packed sequentially (.x .y .z .w .x .y …).
 *   - `indices_vec4` :  same shape; each component holds the morph
 *                       target index for the matching weight slot
 *                       (encoded as a float so it round-trips through
 *                       a vec4 uniform without needing an int slot).
 *   - `active_count` :  number of non-zero entries the shader should
 *                       iterate up to (so the rest of the vec4 padding
 *                       is no-op'd).
 *
 * The bind descriptor is POD; renderer code memcpys it straight into
 * the bgfx uniform array on submit.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_SKINNED_MORPH_BIND_H
#define JCE_SKINNED_MORPH_BIND_H

#include <jce/renderer/jce_skinned_morph_pack.h>

JCE_EXTERN_C_BEGIN

#define JCE_MORPH_BIND_MAX 32  /* must be multiple of 4 */

typedef struct {
    /* weights[i*4..i*4+3] occupy one vec4 uniform slot. */
    float    weights_vec4[JCE_MORPH_BIND_MAX];
    /* indices[i*4..i*4+3] same layout, each component cast from uint8. */
    float    indices_vec4[JCE_MORPH_BIND_MAX];
    uint32_t active_count;     /* 0..JCE_MORPH_BIND_MAX */
    uint32_t vec4_slot_count;  /* ceil(active_count / 4) */
} JceMorphBindDesc;

/* Build the bind descriptor.  `max` is the cap (≤ JCE_MORPH_BIND_MAX,
 * rounded down to a multiple of 4).  Returns false when input is bad. */
JCE_API bool jce_morph_bind_build(const JceMorphSet *set,
                                    const float       *weights,
                                    uint32_t           max,
                                    JceMorphBindDesc  *out);

/* Convenience: zero a desc (used when an entity has no active morphs
 * — the shader still references the uniform array each draw). */
JCE_API void jce_morph_bind_clear(JceMorphBindDesc *desc);

JCE_EXTERN_C_END

#endif /* JCE_SKINNED_MORPH_BIND_H */
