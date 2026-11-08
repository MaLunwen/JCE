#ifndef JCE_SCENE_WATER_MOTION_H
#define JCE_SCENE_WATER_MOTION_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_WATER_MOTION_COMPONENT_VERSION 1u

/* Optional motion controls for FFT Water. Defaults retain the single
 * cascade, original phase rate and original horizontal displacement. */
typedef struct JceWaterMotionComponent {
    uint32_t struct_size;
    uint32_t version;
    float cascade_fraction;             /* secondary cascade fraction [0,0.95] */
    float phase_rate;                   /* phase speed multiplier [0,4] */
    float horizontal_scale;             /* horizontal amplitude multiplier [0,2] */
    float max_horizontal_displacement;  /* maximum horizontal magnitude [0,64] m, 0 uncapped */
} JceWaterMotionComponent;

JCE_API JceWaterMotionComponent JCE_CALL jce_scene_water_motion_default(void);
JCE_API void JCE_CALL jce_scene_set_water_motion(JceScene *scene, JceEntity entity,
    const JceWaterMotionComponent *value);
JCE_API JceWaterMotionComponent *JCE_CALL jce_scene_get_water_motion(
    JceScene *scene, JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_motion(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_motion(JceScene *scene, JceEntity entity);

JCE_EXTERN_C_END
#endif
