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

struct JceModel {
    JceModelNode      *nodes;
    uint32_t           num_nodes;
    JcePbrMaterial    *materials;
    uint32_t           num_materials;
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
