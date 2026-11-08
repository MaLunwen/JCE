#ifndef JCE_SCENE_SKY_CELESTIAL_H
#define JCE_SCENE_SKY_CELESTIAL_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_SCENE_SKY_CELESTIAL_VERSION 1u

/* Optional appearance controls for the physical sky. Sun and moon directions
 * follow the scene's existing time-of-day/environment authority. */
typedef struct JceSceneSkyCelestialSettings {
    uint32_t struct_size;
    uint32_t version;
    float star_intensity;         /* [0,4], default 0 */
    float moon_intensity;         /* [0,4], default 0 */
    float solar_flare_intensity;  /* [0,4], default 0 */
    float moon_phase;             /* lit fraction [0,1], default full (1) */
} JceSceneSkyCelestialSettings;

JCE_API JceSceneSkyCelestialSettings JCE_CALL jce_scene_sky_celestial_default(void);

/* NULL clears the optional block. Invalid values fail without changing scene
 * state. A valid write ensures the scene has rendering settings. */
JCE_API bool JCE_CALL jce_scene_set_sky_celestial_settings(JceScene *scene,
    const JceSceneSkyCelestialSettings *value);

/* Scene-owned value, or NULL when unset. Valid until settings change, the
 * scene clears its rendering settings, or the scene is destroyed. */
JCE_API const JceSceneSkyCelestialSettings *JCE_CALL
jce_scene_get_sky_celestial_settings(const JceScene *scene);

JCE_EXTERN_C_END
#endif
