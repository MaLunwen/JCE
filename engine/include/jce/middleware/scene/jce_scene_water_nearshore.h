#ifndef JCE_SCENE_WATER_NEARSHORE_H
#define JCE_SCENE_WATER_NEARSHORE_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_WATER_NEARSHORE_COMPONENT_VERSION 1u

/* Optional terrain-driven shallow-water region on an FFT Water entity.
 * Terrain 0 leaves the extension inert. The square is centered at the water
 * origin plus the world-space XZ offsets. */
typedef struct JceWaterNearshoreComponent {
    uint32_t struct_size;
    uint32_t version;
    JceEntity terrain;
    float offset_x;            /* metres */
    float offset_z;            /* metres */
    float extent;              /* full side length in metres, [8,128] */
    float drag;                /* flow damping in 1/s, [0,20] */
    float boundary_absorption; /* sponge damping in 1/s, [0,50] */
    float foam_opacity;        /* [0,1] */
} JceWaterNearshoreComponent;

JCE_API JceWaterNearshoreComponent JCE_CALL jce_scene_water_nearshore_default(void);
JCE_API void JCE_CALL jce_scene_set_water_nearshore(JceScene *scene, JceEntity entity,
    const JceWaterNearshoreComponent *value);
JCE_API JceWaterNearshoreComponent *JCE_CALL jce_scene_get_water_nearshore(
    JceScene *scene, JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_nearshore(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_nearshore(JceScene *scene, JceEntity entity);

/* Sample the published nearshore surface blended with the supplied ambient
 * height. Coverage blends the simulated wetness at the region boundary.
 * Returns false and leaves both outputs untouched without an in-range sample. */
JCE_API bool JCE_CALL jce_scene_water_nearshore_surface(const JceScene *scene,
    JceEntity water, float world_x, float world_z, float ambient_y,
    float *surface_y, float *coverage);

JCE_EXTERN_C_END
#endif
