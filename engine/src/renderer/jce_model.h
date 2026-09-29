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

#include <jce/middleware/animation/jce_morph.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_pbr_material.h>
/* Pull the PUBLIC model API in so the narrow morph-walk accessors
 * (jce_model_node_count / _node_prim_count / _prim_vertex_count /
 * _prim_morph) and the morph-aware draw entrypoints
 * (jce_model_draw_morphed / _shadow) have a SINGLE source of truth and can
 * never drift in signature from the internal callers.  This header then only
 * ADDS the struct-typed internal-only accessors below. */
#include <jce/renderer/jce_model.h>

#include "middleware/animation/jce_animation.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JceModel / JceRenderer / JcePakArchive / JceSkinnedMesh / JceSkeleton /
 * JceAnimClip are already typedef'd by the public <jce/renderer/jce_model.h>
 * included above (single declaration — no C99 duplicate-typedef).  Only JceMesh
 * is internal-renderer-only and declared here. */
typedef struct JceMesh         JceMesh;

/* ================================================================== */
/* Model primitive (one draw call unit)                                */
/* ================================================================== */

typedef struct {
    JceMesh        *static_mesh;     /* non-NULL for static geometry */
    JceSkinnedMesh *skinned_mesh;    /* non-NULL for skinned geometry */
    uint32_t        material_index;  /* index into model's material array */
    JceMorphData   *morph;           /* morph-target deltas (FEATURE 3.1), or NULL */
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
/* Model API (internal-only additions)                                 */
/* ================================================================== */
/*
 * The model-loading, lifetime, draw, narrow morph-walk, and morph-aware draw
 * entrypoints are all declared in the PUBLIC <jce/renderer/jce_model.h> pulled
 * in above (single source of truth — no signature drift).  This header ADDS
 * ONLY the struct-typed accessors that intentionally remain internal because
 * they expose the JceModelNode / JcePbrMaterial layouts defined here.
 */

/* Get a node / material by index (returns the internal struct — internal use
 * only; external callers walk the model via the narrow public accessors). */
const JceModelNode   *jce_model_get_node(const JceModel *model, uint32_t index);

uint32_t              jce_model_material_count(const JceModel *model);
const JcePbrMaterial *jce_model_get_material(const JceModel *model, uint32_t index);

/* jce_model_morph_anim_count / _track moved to the PUBLIC header, which this
 * one includes: they hand back opaque things, and the scene renderer needs
 * them from outside engine/src/renderer/ -- where it was reaching them
 * through an implicit declaration that truncated the returned pointer. */

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_H */
