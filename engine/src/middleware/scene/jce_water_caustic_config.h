#ifndef JCE_WATER_CAUSTIC_CONFIG_H
#define JCE_WATER_CAUSTIC_CONFIG_H

#include <jce/middleware/scene/jce_scene_water_caustics.h>

static inline bool jce_water_caustics_valid(const JceWaterCausticsComponent *p)
{
    return p && p->struct_size >= sizeof(*p) &&
        p->version == JCE_WATER_CAUSTICS_COMPONENT_VERSION &&
        p->offset_x >= -10.0f && p->offset_x <= 10.0f &&
        p->offset_z >= -10.0f && p->offset_z <= 10.0f &&
        p->fade_depth >= 0.01f && p->fade_depth <= 1000.0f;
}
#endif
