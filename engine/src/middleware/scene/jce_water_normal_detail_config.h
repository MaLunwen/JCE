#ifndef JCE_WATER_NORMAL_DETAIL_CONFIG_H
#define JCE_WATER_NORMAL_DETAIL_CONFIG_H

#include <jce/middleware/scene/jce_scene_water_normal_detail.h>

static inline bool jce_water_normal_detail_valid(const JceWaterNormalDetailComponent *value)
{
    return value && value->struct_size >= sizeof(*value) &&
        value->version == JCE_WATER_NORMAL_DETAIL_COMPONENT_VERSION &&
        value->frequency_low >= 0.1f && value->frequency_low <= 64.0f &&
        value->frequency_high >= 0.1f && value->frequency_high <= 64.0f &&
        value->strength >= 0.0f && value->strength <= 1.0f;
}

#endif
