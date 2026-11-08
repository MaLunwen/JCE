#ifndef JCE_SCENE_SKY_CELESTIAL_INTERNAL_H
#define JCE_SCENE_SKY_CELESTIAL_INTERNAL_H

#include <jce/middleware/scene/jce_scene_sky_celestial.h>

typedef struct cJSON cJSON;

typedef struct JceSceneSkyNightStyle {
    float moon_radius_degrees; /* 0 uses the stock angular radius */
    float gradient_strength;  /* 0 keeps the stock night sky */
} JceSceneSkyNightStyle;

JceSceneSkyNightStyle jce_scene_sky_night_style(const JceScene *scene);

bool jce_scene_sky_celestial_write_json(cJSON *sky,
    const JceScene *scene);
/* Absent/null returns a version-zero value. Malformed returns false. */
bool jce_scene_sky_celestial_read_json(const cJSON *rendering,
    JceSceneSkyCelestialSettings *out, JceSceneSkyNightStyle *style);

#endif
