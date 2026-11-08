#ifndef JCE_WATER_CAUSTIC_DETAIL_CONFIG_H
#define JCE_WATER_CAUSTIC_DETAIL_CONFIG_H

#include <jce/middleware/scene/jce_scene_water_caustic_detail.h>

static inline bool jce_water_caustic_detail_valid(const JceWaterCausticDetailComponent *value)
{
    return value && value->struct_size >= sizeof(*value) &&
        value->version == JCE_WATER_CAUSTIC_DETAIL_COMPONENT_VERSION &&
        value->secondary_weight >= 0.0f && value->secondary_weight <= 1.0f &&
        value->transition_start_m >= 0.0f && value->transition_start_m <= 80.0f &&
        value->transition_end_m > value->transition_start_m &&
        value->transition_end_m <= 100.0f &&
        value->tile_size_m >= 0.0f && value->tile_size_m <= 128.0f &&
        value->fine_period_ratio >= 0.0f && value->fine_period_ratio <= 0.5f &&
        value->fine_weight >= 0.0f && value->fine_weight <= 1.0f &&
        value->fine_fade_depth_m >= 0.05f && value->fine_fade_depth_m <= 100.0f;
}

#endif
