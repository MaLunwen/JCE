#ifndef JCE_WATER_NEARSHORE_CONFIG_H
#define JCE_WATER_NEARSHORE_CONFIG_H

#include <float.h>
#include <jce/middleware/scene/jce_scene_water_nearshore.h>

/* Existing small regions retain their 32-cell discretization. Only an
 * explicitly authored region wider than 32 metres requests the bounded 64 grid. */
static inline int jce_water_nearshore_grid_side(float extent)
{
    return extent > 32.0f ? 64 : 32;
}

static inline bool jce_water_nearshore_valid(const JceWaterNearshoreComponent *value)
{
    return value && value->struct_size >= sizeof(*value) &&
        value->version == JCE_WATER_NEARSHORE_COMPONENT_VERSION &&
        value->offset_x >= -FLT_MAX && value->offset_x <= FLT_MAX &&
        value->offset_z >= -FLT_MAX && value->offset_z <= FLT_MAX &&
        value->extent >= 8.0f && value->extent <= 128.0f &&
        value->drag >= 0.0f && value->drag <= 20.0f &&
        value->boundary_absorption >= 0.0f && value->boundary_absorption <= 50.0f &&
        value->foam_opacity >= 0.0f && value->foam_opacity <= 1.0f;
}

#endif
