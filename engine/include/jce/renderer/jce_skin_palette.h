/*
 * jce_skin_palette.h  Pure helper: build the world-space bone palette.
 *
 * Shared by the color pass (jce_model_draw) and the shadow/depth pass
 * (jce_model_draw_shadow) so both deform skinned geometry identically.
 *
 * Layer: Renderer (Layer 3) — pure math, no bgfx.
 */

#ifndef JCE_SKIN_PALETTE_H
#define JCE_SKIN_PALETTE_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Build the world-space bone palette: out[i] = root * joints[i].
 *
 * Pre-multiplying every joint by the model-to-world root lets the skinned
 * vertex shader emit world-space positions directly — the same convention
 * jce_model_draw() uses for the color pass, so the shadow pass deforms the
 * silhouette in lock-step with the lit mesh.
 *
 * root:       model-to-world matrix; NULL is treated as identity (joints
 *             are copied through unchanged).
 * joints:     source bone palette (globalTransform * inverseBind per joint).
 * num_joints: number of source matrices.
 * out:        destination buffer.
 * out_cap:    capacity of out; the count is clamped to this.
 *
 * Returns the number of matrices written = min(num_joints, out_cap), or 0
 * when joints/out is NULL or either count is 0. */
JCE_API uint32_t jce_skin_build_world_palette(const jce_mat4 *root,
                                      const jce_mat4 *joints,
                                      uint32_t num_joints,
                                      jce_mat4 *out,
                                      uint32_t out_cap);

JCE_EXTERN_C_END

#endif /* JCE_SKIN_PALETTE_H */
