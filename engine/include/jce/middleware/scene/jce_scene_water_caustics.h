#ifndef JCE_SCENE_WATER_CAUSTICS_H
#define JCE_SCENE_WATER_CAUSTICS_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_WATER_CAUSTICS_COMPONENT_VERSION 1u

/* RGB projection from the primary FFT displacement grid. Uses the Water
 * material's existing caustic strength and requires positive clarity. */
typedef struct JceWaterCausticsComponent {
    uint32_t struct_size;
    uint32_t version;
    float offset_x;   /* horizontal channel sampling offset in metres, [-10,10] */
    float offset_z;   /* horizontal channel sampling offset in metres, [-10,10] */
    float fade_depth; /* receiver depth at zero effect in metres, [0.01,1000] */
} JceWaterCausticsComponent;

JCE_API JceWaterCausticsComponent JCE_CALL jce_scene_water_caustics_default(void);
JCE_API void JCE_CALL jce_scene_set_water_caustics(JceScene *scene, JceEntity entity,
    const JceWaterCausticsComponent *value);
JCE_API JceWaterCausticsComponent *JCE_CALL jce_scene_get_water_caustics(JceScene *scene,
    JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_caustics(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_caustics(JceScene *scene, JceEntity entity);

JCE_EXTERN_C_END
#endif
