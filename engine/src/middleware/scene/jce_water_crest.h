#ifndef JCE_WATER_CREST_INTERNAL_H
#define JCE_WATER_CREST_INTERNAL_H
#include <jce/middleware/scene/jce_scene_water_crest.h>

static inline bool jce_water_crest_valid(const JceWaterCrestComponent *value)
{
    /* Ordered bounds reject NaN and infinity. */
    if (!value || value->struct_size < sizeof(*value) ||
        value->version != JCE_WATER_CREST_COMPONENT_VERSION ||
        !(value->start_height >= 0.0f && value->start_height <= 1000.0f) ||
        !(value->height_range >= 0.001f && value->height_range <= 1000.0f) ||
        !(value->fade_distance >= 0.1f && value->fade_distance <= 100000.0f) ||
        !(value->strength >= 0.0f && value->strength <= 8.0f)) return false;
    for (int i = 0; i < 3; ++i)
        if (!(value->tint[i] >= 0.0f && value->tint[i] <= 1.0f)) return false;
    return true;
}
#endif
