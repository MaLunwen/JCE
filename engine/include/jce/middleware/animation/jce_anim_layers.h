/*
 * jce_anim_layers.h  Multi-layer animation stack (Unity AnimatorLayer).
 *
 * A layer stack composes multiple clips on the same skeleton with
 * per-layer weight, blend mode, and optional per-bone weight mask.
 * Final pose is base + Σ(weight_i × mask_i × layer_pose_i).
 *
 * Typical use case: third-person character with a "base" walk/run loop
 * on the lower body and an "aim" override on the upper body controlled
 * by a bone mask covering spine/arms/head only.  Mirrors Unity's
 * AnimatorController layer list with per-layer Avatar Mask.
 *
 * The stack is owned by the caller (NOT by JceAnimPlayer) — typical
 * pattern: one stack per animated entity.  Clips and skeleton are
 * borrowed; the stack does not assume ownership.
 *
 * Layer: middleware (Layer 3) — public.
 */

#ifndef JCE_ANIM_LAYERS_H
#define JCE_ANIM_LAYERS_H

#include <jce/middleware/animation/jce_animation.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAnimLayerStack JceAnimLayerStack;

typedef enum {
    /* result = lerp(result, layer, weight * mask[i])  — Unity default */
    JCE_ANIM_BLEND_OVERRIDE = 0,
    /* Currently identical to OVERRIDE; reserved for true additive
     * (delta-from-rest) once authoring conventions are settled. */
    JCE_ANIM_BLEND_ADDITIVE = 1,
} JceAnimBlendMode;

#define JCE_ANIM_LAYER_MAX 8

/* Create a stack sized to a specific skeleton.  num_joints must match
 * every clip / mask used with this stack.  Caller owns the returned
 * pointer; free with jce_anim_layer_stack_destroy. */
JCE_API JceAnimLayerStack *jce_anim_layer_stack_create(uint32_t num_joints);

JCE_API void               jce_anim_layer_stack_destroy(JceAnimLayerStack *stack);

JCE_API uint32_t           jce_anim_layer_stack_count(const JceAnimLayerStack *stack);

/* Configure a single layer.  `clip` may be NULL to disable the layer
 * (it is then skipped during evaluate).  `time` is in seconds into the
 * clip.  `weight` is in [0, 1].  Returns false if idx out of range. */
JCE_API bool jce_anim_layer_set(JceAnimLayerStack *stack, uint32_t idx,
                                const JceAnimClip *clip,
                                float time, float weight,
                                JceAnimBlendMode mode);

/* Bone weight mask (length = num_joints).  Pass NULL to clear (full
 * body).  The stack copies the buffer internally — caller can free. */
JCE_API bool jce_anim_layer_set_bone_mask(JceAnimLayerStack *stack,
                                           uint32_t idx,
                                           const float *bone_weights);

/* Compose all active layers on top of the base pose.  `base_locals`
 * (size num_joints) is the starting pose; `out_locals` (size
 * num_joints) receives the composed pose.  `base_locals` and
 * `out_locals` may alias.  Sampling is done internally using the
 * skeleton's rest TRS for clip inversion. */
JCE_API void jce_anim_layer_stack_evaluate(JceAnimLayerStack    *stack,
                                            const JceSkeleton    *skel,
                                            const jce_mat4       *base_locals,
                                            jce_mat4             *out_locals);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_LAYERS_H */
