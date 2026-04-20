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

#include <jce/core/jce_math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceModel    JceModel;
typedef struct JceRenderer JceRenderer;
typedef struct JcePakArchive  JcePakArchive;
typedef struct JceSkeleton JceSkeleton;
typedef struct JceAnimClip JceAnimClip;

/* Load a glTF/GLB model from the PAK archive.
   Returns NULL on failure (asset not found, parse error, OOM). */
JceModel *jce_model_load_gltf(const JcePakArchive *pak, const char *asset_path);

/* Load a glTF/GLB model from raw file bytes in memory.
   name is used for logging only; may be NULL. */
JceModel *jce_model_load_gltf_memory(const void *data, uint32_t size,
                                      const char *name);

/* Destroy a model and all owned GPU resources (meshes, textures, skeleton). */
void jce_model_destroy(JceModel *model);

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

/* -- Skeleton & animation accessors -------------------------------- */

/* Returns the skeleton, or NULL if the model has no skinning. */
JceSkeleton  *jce_model_get_skeleton(const JceModel *model);

/* Number of animation clips embedded in the model. */
uint32_t      jce_model_anim_count(const JceModel *model);

/* Get animation clip by index. Returns NULL if out of range. */
JceAnimClip  *jce_model_get_anim(const JceModel *model, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_PUBLIC_H */
