#ifndef JCE_WATER_MOTION_CONFIG_H
#define JCE_WATER_MOTION_CONFIG_H

#include <jce/middleware/scene/jce_scene_water_motion.h>

static inline bool jce_water_motion_valid(const JceWaterMotionComponent *value)
{
    return value && value->struct_size >= sizeof(*value) &&
        value->version == JCE_WATER_MOTION_COMPONENT_VERSION &&
        value->cascade_fraction >= 0.0f && value->cascade_fraction <= 0.95f &&
        value->phase_rate >= 0.0f && value->phase_rate <= 4.0f &&
        value->horizontal_scale >= 0.0f && value->horizontal_scale <= 2.0f &&
        value->max_horizontal_displacement >= 0.0f &&
        value->max_horizontal_displacement <= 64.0f;
}

#endif
