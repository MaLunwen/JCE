#ifndef JCE_SCENE_WATER_OPTICS_H
#define JCE_SCENE_WATER_OPTICS_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_WATER_OPTICS_COMPONENT_VERSION 1u

/* Optional screen-space optics for physical water on a supported viewport.
 * A zero strength keeps the corresponding legacy shading path. */
typedef struct JceWaterOpticsComponent {
    uint32_t struct_size;
    uint32_t version;
    float ssr_strength;         /* screen-space reflection strength, [0,1] */
    float refraction_strength;  /* scene-color refraction strength, [0,1] */
} JceWaterOpticsComponent;

JCE_API JceWaterOpticsComponent JCE_CALL jce_scene_water_optics_default(void);
JCE_API void JCE_CALL jce_scene_set_water_optics(JceScene *scene, JceEntity entity,
    const JceWaterOpticsComponent *value);
JCE_API JceWaterOpticsComponent *JCE_CALL jce_scene_get_water_optics(
    JceScene *scene, JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_optics(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_optics(JceScene *scene, JceEntity entity);

JCE_EXTERN_C_END
#endif
