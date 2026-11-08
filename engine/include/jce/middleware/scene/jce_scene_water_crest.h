#ifndef JCE_SCENE_WATER_CREST_H
#define JCE_SCENE_WATER_CREST_H
#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_WATER_CREST_COMPONENT_VERSION 1u

/* Optional transmitted crest light for physical Water materials. */
typedef struct JceWaterCrestComponent {
    uint32_t struct_size;
    uint32_t version;
    float start_height;   /* metres above still water, [0,1000] */
    float height_range;   /* metres from first light to full weight, [0.001,1000] */
    float fade_distance;  /* metres from the camera at zero weight, [0.1,100000] */
    float strength;       /* [0,8]; zero disables the contribution */
    float tint[3];        /* linear RGB, each channel in [0,1] */
} JceWaterCrestComponent;

JCE_API JceWaterCrestComponent JCE_CALL jce_scene_water_crest_default(void);
JCE_API void JCE_CALL jce_scene_set_water_crest(JceScene *scene, JceEntity entity,
    const JceWaterCrestComponent *value);
JCE_API JceWaterCrestComponent *JCE_CALL jce_scene_get_water_crest(JceScene *scene, JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_crest(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_crest(JceScene *scene, JceEntity entity);

JCE_EXTERN_C_END
#endif
