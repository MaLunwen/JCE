#ifndef JCE_WATER_OPTICS_CONFIG_H
#define JCE_WATER_OPTICS_CONFIG_H

#include <jce/middleware/scene/jce_scene_water_optics.h>

static inline bool jce_water_optics_valid(const JceWaterOpticsComponent *value)
{
    return value && value->struct_size >= sizeof(*value) &&
        value->version == JCE_WATER_OPTICS_COMPONENT_VERSION &&
        value->ssr_strength >= 0.0f && value->ssr_strength <= 1.0f &&
        value->refraction_strength >= 0.0f && value->refraction_strength <= 1.0f;
}

#endif
