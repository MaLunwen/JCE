/*
 * jce_anim_ozz.h  Internal bridge to ozz-animation runtime.
 *
 * Declares extern "C" functions that wrap ozz-animation's optimized
 * job-based API (sampling, local-to-model, blending).  The implementation
 * lives in jce_anim_ozz.cpp (C++20); every other translation unit
 * includes this header as plain C.
 *
 * Layer: Animation (Layer 3) -- internal.
 */

#ifndef JCE_ANIM_OZZ_H
#define JCE_ANIM_OZZ_H

#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Opaque handles to ozz runtime objects.                              */
/* ================================================================== */

typedef struct JceOzzSkeleton  JceOzzSkeleton;
typedef struct JceOzzAnimation JceOzzAnimation;
typedef struct JceOzzContext   JceOzzContext;

/* ================================================================== */
/* Skeleton operations                                                 */
/* ================================================================== */

/* Build an ozz-side skeleton mirror from JCE joint data.
 * rest_locals : [num_joints] rest-pose local transforms (column-major mat4).
 * parents     : [num_joints] parent indices (-1 = root).
 * The arrays are copied; caller retains ownership. */
JceOzzSkeleton *jce_ozz_skeleton_create(const jce_mat4 *rest_locals,
                                        const int      *parents,
                                        uint32_t        num_joints);

void jce_ozz_skeleton_destroy(JceOzzSkeleton *s);

/* Evaluate local transforms into model-space skinning matrices.
 * local_transforms : [num_joints] per-joint local transforms.
 * out_model_matrices : [max_joints] output (global * inv-bind would be
 *                      applied by the caller -- this function only
 *                      computes the global hierarchy product).
 * The function walks the parent chain using ozz math utilities. */
void jce_ozz_skeleton_evaluate(JceOzzSkeleton  *s,
                               const jce_mat4  *local_transforms,
                               jce_mat4        *out_model_matrices,
                               uint32_t         max_joints);

/* ================================================================== */
/* Sampling context (one per animation player)                         */
/* ================================================================== */

JceOzzContext *jce_ozz_context_create(uint32_t max_joints);
void           jce_ozz_context_destroy(JceOzzContext *ctx);

/* Sample an animation clip.
 * timestamps / translations / rotations / scales define channels per joint.
 * Output is written to out_locals as composed mat4 transforms. */
void jce_ozz_sample(JceOzzContext   *ctx,
                    float            time,
                    float            duration,
                    const float     *timestamps,
                    uint32_t         num_keyframes,
                    const jce_vec3  *rest_t,
                    const jce_quat  *rest_r,
                    const jce_vec3  *rest_s,
                    jce_mat4        *out_locals,
                    uint32_t         num_joints);

/* ================================================================== */
/* Blending                                                            */
/* ================================================================== */

/* Linearly blend two sets of local transforms.
 * out[i] = lerp(a[i], b[i], weight)  (component-wise on the 4x4). */
void jce_ozz_blend(const jce_mat4 *a,
                   const jce_mat4 *b,
                   float           weight,
                   jce_mat4       *out,
                   uint32_t        num_joints);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ANIM_OZZ_H */
