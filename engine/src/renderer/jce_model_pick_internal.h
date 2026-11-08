/* Renderer-owned CPU picking for resolved model geometry. */
#ifndef JCE_MODEL_PICK_INTERNAL_H
#define JCE_MODEL_PICK_INTERNAL_H

#include <jce/renderer/jce_model.h>

typedef enum JceModelRayResult {
    JCE_MODEL_RAY_UNAVAILABLE,
    JCE_MODEL_RAY_MISS,
    JCE_MODEL_RAY_HIT,
} JceModelRayResult;

JceModelRayResult jce_model_pick_ray(const JceModel *model,
    const jce_mat4 *world, const jce_mat4 *palette, uint32_t joints,
    const float *weights, uint32_t weight_count,
    jce_vec3 origin, jce_vec3 direction, float max_t, float *out_t);

#endif
