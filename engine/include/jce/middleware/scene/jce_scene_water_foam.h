#ifndef JCE_SCENE_WATER_FOAM_H
#define JCE_SCENE_WATER_FOAM_H
#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN
#define JCE_WATER_FOAM_COMPONENT_VERSION 1u
/* Optional history layer for an enabled WaterSpectrum FFT body. It does not
 * alter ambient displacement or the absolute-time sampling contract. */
typedef struct JceWaterFoamComponent {
    uint32_t struct_size;
    uint32_t version;
    float generation_rate; /* 1/s, [0,100] */
    float decay_rate;      /* 1/s, [0,100] */
    float fold_threshold;  /* [-1,1]; zero emits only for negative Jacobian */
    float opacity;         /* [0,1] */
    float texture_scale;   /* inverse metres, [0.0001,1000] */
} JceWaterFoamComponent;
JCE_API JceWaterFoamComponent JCE_CALL jce_scene_water_foam_default(void);
JCE_API void JCE_CALL jce_scene_set_water_foam(JceScene *scene, JceEntity entity,
    const JceWaterFoamComponent *value);
JCE_API JceWaterFoamComponent *JCE_CALL jce_scene_get_water_foam(JceScene *scene, JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_foam(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_foam(JceScene *scene, JceEntity entity);
JCE_EXTERN_C_END
#endif
