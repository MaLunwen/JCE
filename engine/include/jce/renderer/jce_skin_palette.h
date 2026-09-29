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
/* Where a BONE is, in world space -- recovered from the palette the renderer
 * already has.
 *
 * A skinning palette entry is `global * inverseBind`: it takes a vertex from
 * mesh space to its deformed position, which is not a pose and cannot be
 * attached to.  The bone's own transform is what a weapon in a hand or a
 * jetpack on a spine needs, and jce_skeleton.h:92 has stated how to get it
 * back since it was written:
 *
 *     joint_global = skin_matrix[i] * inverse(inverse_bind[i])
 *
 * Nothing in this engine used that sentence.  This is it, with the root
 * folded in so the result is world and not model space.
 *
 * `root` is the model-to-world matrix (NULL = identity, same convention as
 * jce_skin_build_world_palette).  `inverse_bind` is that ONE bone's inverse
 * bind matrix -- jce_skeleton_get_inverse_bind(skel, bone_index).
 *
 * A BONE PAST THE END OF THE PALETTE FALLS BACK TO ITS BIND POSE RATHER THAN
 * FAILING, and so does a NULL/empty palette, because `skin_palette_count == 0`
 * is how this renderer says "draw the bind pose" -- a model with a skeleton
 * and no clip playing.  An attachment on such a model should sit where the
 * bone rests, not vanish or report an error the caller has no answer for.
 *
 * Costs one 4x4 inverse per call.  That is per QUERY, not per bone per frame:
 * the palette is already computed and nothing new is stored. */
JCE_API bool jce_skin_bone_world(const jce_mat4 *root,
                                 const jce_mat4 *palette,
                                 uint32_t palette_count,
                                 const jce_mat4 *inverse_bind,
                                 uint32_t bone_index,
                                 jce_mat4 *out_world);

JCE_API uint32_t jce_skin_build_world_palette(const jce_mat4 *root,
                                      const jce_mat4 *joints,
                                      uint32_t num_joints,
                                      jce_mat4 *out,
                                      uint32_t out_cap);

JCE_EXTERN_C_END

#endif /* JCE_SKIN_PALETTE_H */
