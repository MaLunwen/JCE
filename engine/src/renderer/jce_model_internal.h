/*
 * jce_model_internal.h  Shared definition of struct JceModel.
 *
 * Included by jce_model.c (accessors / draw / destroy) and by
 * jce_gltf_loader.c (construction).  Not for use outside the engine.
 *
 * Layer: Graphics (Layer 3) -- internal.
 */
#ifndef JCE_MODEL_INTERNAL_H
#define JCE_MODEL_INTERNAL_H

#include "jce_model.h"
#include "jce_model_texture_pool.h"

#include <jce/middleware/animation/jce_morph.h>

/* One imported glTF "weights" animation channel: a keyframed morph-weight
 * track bound to the model node whose mesh it drives (FEATURE 3.1).  glTF
 * weights channels are dropped from the skeletal JceAnimClip (which only
 * carries joint TRS) and collected here instead. */
typedef struct {
    uint32_t             anim_index;   /* index into anim_clips[] */
    uint32_t             node_index;   /* index into nodes[] (mesh owner) */
    JceMorphWeightTrack *track;        /* owned */
} JceModelMorphAnim;

/* Shared by rigid draws and CPU picking, including bone attachments. */
jce_mat4 jce_model_static_node_world(const JceModel *model,
    const JceModelNode *node, const jce_mat4 *root,
    const jce_mat4 *fallback_world, const jce_mat4 *joint_matrices,
    uint32_t num_joints);

/* Imported opt-in atlas metadata, parallel to the model's material slots. */
typedef struct {
    JceTexture texture; /* borrowed from the model texture pool */
    int16_t head_joint; /* skin.joints index, -1 for no face lookup */
    bool active;
    bool hair_caster;
} JceModelToonAtlas;

struct JceModel {
    JceModelNode      *nodes;
    uint32_t           num_nodes;
    JcePbrMaterial    *materials;
    uint32_t           num_materials;
    JceModelToonAtlas *toon_atlases;
    JceModelTexturePool *texture_pool; /* owns maps borrowed by materials */
    JceSkeleton       *skeleton;       /* NULL if no skinning */
    JceAnimClip      **anim_clips;
    uint32_t           num_anims;
    JceModelMorphAnim *morph_anims;    /* morph-weight tracks, or NULL */
    uint32_t           num_morph_anims;

    /* Local-space bounding box (model space; node transforms baked in), computed
     * once at upload from all primitive vertex positions.  Used by the scene
     * renderer's frustum cull so model-path entities cull by their real extent
     * rather than a point at the origin.  has_aabb == false => not computed. */
    float              aabb_min[3];
    float              aabb_max[3];
    bool               has_aabb;
};

#endif /* JCE_MODEL_INTERNAL_H */
