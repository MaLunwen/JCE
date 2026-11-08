#ifndef JCE_SCENE_SKY_EXTRAS_JSON_H
#define JCE_SCENE_SKY_EXTRAS_JSON_H

#include "jce_scene_sky_celestial_internal.h"
#include <jce/middleware/scene/jce_scene_cloud_shape.h>

typedef struct JceSceneSkyExtras {
    JceSceneSkyCelestialSettings celestial;
    JceSceneSkyNightStyle night_style;
    JceSceneCloudShapeSettings cloud_shape;
} JceSceneSkyExtras;

bool jce_scene_sky_extras_write_json(cJSON *sky, const JceScene *scene);
bool jce_scene_sky_extras_read_json(const cJSON *rendering, JceSceneSkyExtras *out);
void jce_scene_sky_extras_apply(JceScene *scene, const JceSceneSkyExtras *value);

#endif
