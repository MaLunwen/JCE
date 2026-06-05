/*
 * jce_model.h  Public API for loading and drawing composite glTF models.
 *
 * A JceModel encapsulates meshes, PBR materials, an optional skeleton,
 * and animation clips loaded from a GLB/glTF file in the PAK archive.
 *
 * Layer: Graphics (Layer 3) — public.
 */

#ifndef JCE_MODEL_PUBLIC_H
#define JCE_MODEL_PUBLIC_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceModel    JceModel;
typedef struct JceRenderer JceRenderer;
typedef struct JcePakArchive  JcePakArchive;
typedef struct JceSkeleton JceSkeleton;
typedef struct JceAnimClip JceAnimClip;

/* Load a glTF/GLB model from the PAK archive.
   Returns NULL on failure (asset not found, parse error, OOM). */
JCE_API JceModel *jce_model_load_gltf(const JcePakArchive *pak, const char *asset_path);

/* Load a glTF/GLB model from raw file bytes in memory.
   name is used for logging only; may be NULL. */
JceModel *jce_model_load_gltf_memory(const void *data, uint32_t size,
                                      const char *name);

/* Destroy a model and all owned GPU resources (meshes, textures, skeleton). */
JCE_API void jce_model_destroy(JceModel *model);

/* Draw all mesh primitives with their PBR materials.
 *
 * transform:      model-to-world matrix applied to every node (required).
 * joint_matrices: bone palette for skinned meshes; pass NULL for static models.
 * num_joints:     number of matrices in joint_matrices (0 for static models). */
void jce_model_draw(const JceModel *model,
                    const JceRenderer *r, uint16_t view_id,
                    const jce_mat4 *transform,
                    const jce_mat4 *joint_matrices,
                    uint32_t num_joints);

/* Draw all primitives into a shadow/depth pass (depth-only, no materials).
 *
 * Skinned primitives reuse the same world-space bone palette the color
 * pass uploads, so an animated model casts a shadow that follows its
 * skeleton.  Pass NULL/0 joints to rasterize the bind pose.
 *
 * view_id must be a shadow producer view (single map or a CSM cascade). */
void jce_model_draw_shadow(const JceModel *model,
                           const JceRenderer *r, uint16_t view_id,
                           const jce_mat4 *transform,
                           const jce_mat4 *joint_matrices,
                           uint32_t num_joints);

/* Submit a wireframe overlay of every primitive in the model, walking
 * the full node hierarchy. Used by editor tooling (selection outlines)
 * to draw the true geometric silhouette of skinned/static models that
 * already had their materials/lights bound by the regular pass.
 *
 * Pass joint_matrices == NULL for the bind pose (skinned meshes only);
 * otherwise pass the live bone palette so the wireframe deforms with
 * the current animation. */
JCE_API void jce_model_submit_wireframe_overlay(const JceModel *model,
                                                const JceRenderer *r,
                                                uint16_t view_id,
                                                const jce_mat4 *transform,
                                                const jce_mat4 *joint_matrices,
                                                uint32_t num_joints);

/* Submit every primitive to an object-ID picking pass.
 * Caller must set the object-ID uniform before calling.  The function walks
 * the same node hierarchy as jce_model_draw() and respects per-material
 * double-sided culling. */
JCE_API void jce_model_submit_pick_id(const JceModel *model,
                                      const JceRenderer *r,
                                      uint16_t view_id,
                                      const jce_mat4 *transform,
                                      const jce_mat4 *joint_matrices,
                                      uint32_t num_joints,
                                      JceShaderHandle static_program,
                                      JceShaderHandle skinned_program);

/* -- Skeleton & animation accessors -------------------------------- */

/* Returns the skeleton, or NULL if the model has no skinning. */
JCE_API JceSkeleton  *jce_model_get_skeleton(const JceModel *model);

/* Number of animation clips embedded in the model. */
JCE_API uint32_t      jce_model_anim_count(const JceModel *model);

/* Get animation clip by index. Returns NULL if out of range. */
JCE_API JceAnimClip  *jce_model_get_anim(const JceModel *model, uint32_t index);

JCE_EXTERN_C_END

#endif /* JCE_MODEL_PUBLIC_H */
