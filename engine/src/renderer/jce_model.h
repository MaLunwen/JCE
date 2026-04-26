/*
 * jce_model.h  Composite 3D model (meshes + materials + skeleton + animations).
 *
 * A JceModel represents a loaded glTF scene: multiple mesh primitives,
 * each with an associated PBR material, an optional skeleton, and
 * zero or more animation clips.
 *
 * Layer: Graphics (Layer 3).
 */

#ifndef JCE_MODEL_H
#define JCE_MODEL_H

#include <jce/renderer/jce_pbr_material.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include "middleware/animation/jce_animation.h"
#include <jce/os/core/jce_math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceMesh         JceMesh;
typedef struct JceSkinnedMesh  JceSkinnedMesh;
typedef struct JceRenderer     JceRenderer;
typedef struct JcePakArchive      JcePakArchive;
typedef struct JceModel        JceModel;

/* ================================================================== */
/* Model primitive (one draw call unit)                                */
/* ================================================================== */

typedef struct {
    JceMesh        *static_mesh;     /* non-NULL for static geometry */
    JceSkinnedMesh *skinned_mesh;    /* non-NULL for skinned geometry */
    uint32_t        material_index;  /* index into model's material array */
} JceModelPrimitive;

/* ================================================================== */
/* Model node (one node in the scene hierarchy)                        */
/* ================================================================== */

typedef struct {
    char                name[64];
    jce_mat4            local_transform;
    JceModelPrimitive  *primitives;
    uint32_t            num_primitives;
    int16_t             parent;               /* -1 = root */
    int32_t             joint_parent_index;   /* -1, or index in skin joints[] if this
                                                 static mesh is a direct child of a joint */
    jce_mat4            joint_local_matrix;   /* node's local TRS relative to parent joint
                                                 (only valid when joint_parent_index >= 0) */
} JceModelNode;

/* ================================================================== */
/* Model API                                                           */
/* ================================================================== */

/* Load a glTF/GLB model from PAK. Returns NULL on failure. */
JceModel *jce_model_load_gltf(const JcePakArchive *pak, const char *asset_path);

/* Destroy a model and all owned resources (meshes, textures, skeleton, anims). */
void jce_model_destroy(JceModel *model);

/* -- Accessors ------------------------------------------------------ */

uint32_t              jce_model_node_count(const JceModel *model);
const JceModelNode   *jce_model_get_node(const JceModel *model, uint32_t index);

uint32_t              jce_model_material_count(const JceModel *model);
const JcePbrMaterial *jce_model_get_material(const JceModel *model, uint32_t index);

/* Skeleton (NULL if model has no skinning). */
JceSkeleton          *jce_model_get_skeleton(const JceModel *model);

/* Animation clips. */
uint32_t              jce_model_anim_count(const JceModel *model);
JceAnimClip          *jce_model_get_anim(const JceModel *model, uint32_t index);

/* -- Rendering ------------------------------------------------------ */

/* Draw all primitives with their PBR materials.
 * transform:       model-to-world matrix (for static meshes).
 * joint_matrices:  bone palette (for skinned meshes), or NULL.
 * num_joints:      number of matrices in joint_matrices. */
void jce_model_draw(const JceModel *model,
                     const JceRenderer *r, uint16_t view_id,
                     const jce_mat4 *transform,
                     const jce_mat4 *joint_matrices,
                     uint32_t num_joints);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_H */
