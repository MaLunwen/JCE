/*
 * jce_mpb_apply.h  Flatten a Material Property Block into a draw-
 * call-ready uniform list.
 *
 * The renderer's draw path takes a JceMpbUniformList per renderer
 * instance and rebinds each entry as a vec4 uniform before submit.
 * This module converts a JceMaterialPropertyBlock (B4 data layer)
 * into that flat list.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_MPB_APPLY_H
#define JCE_MPB_APPLY_H

#include <jce/renderer/jce_material_property_block.h>
#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_MPB_APPLY_MAX_UNIFORMS 16
#define JCE_MPB_APPLY_NAME_LEN     32

typedef enum {
    JCE_MPB_UNIFORM_VEC4    = 0,    /* float/vec3/vec4/color packed */
    JCE_MPB_UNIFORM_TEXTURE = 1,    /* texture id in value[0] */
} JceMpbUniformKind;

typedef struct {
    char              name[JCE_MPB_APPLY_NAME_LEN];
    JceMpbUniformKind kind;
    float             value[4];     /* vec4 OR texture handle in value[0] */
} JceMpbUniformEntry;

typedef struct {
    JceMpbUniformEntry entries[JCE_MPB_APPLY_MAX_UNIFORMS];
    uint32_t           count;
} JceMpbUniformList;

/* Flatten `block` into `out`.  `out` is cleared first.  Returns the
 * number of entries written.  Empty / NULL block returns 0. */
JCE_API uint32_t jce_mpb_apply_flatten(const JceMaterialPropertyBlock *block,
                                        JceMpbUniformList *out);

JCE_EXTERN_C_END

#endif /* JCE_MPB_APPLY_H */
