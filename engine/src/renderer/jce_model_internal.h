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

struct JceModel {
    JceModelNode    *nodes;
    uint32_t         num_nodes;
    JcePbrMaterial  *materials;
    uint32_t         num_materials;
    JceSkeleton     *skeleton;       /* NULL if no skinning */
    JceAnimClip    **anim_clips;
    uint32_t         num_anims;
};

#endif /* JCE_MODEL_INTERNAL_H */
