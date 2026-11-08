#ifndef JCE_SCENE_WATER_CAUSTIC_DETAIL_H
#define JCE_SCENE_WATER_CAUSTIC_DETAIL_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

#define JCE_WATER_CAUSTIC_DETAIL_COMPONENT_VERSION 1u

/* Optional detail for a projected WaterCaustics component. A zero secondary
 * weight disables the second scale. A zero tile size selects single sampling. */
typedef struct JceWaterCausticDetailComponent {
    uint32_t struct_size;
    uint32_t version;
    float secondary_weight;     /* [0,1] */
    float transition_start_m;   /* [0,80] */
    float transition_end_m;     /* (transition_start_m,100] */
    float tile_size_m;          /* [0,128]; positive sizes below 0.05 m render at 0.05 m */
    float fine_period_ratio;   /* [0,0.5] of the primary period, 0 disables */
    float fine_weight;         /* [0,1], 0 disables */
    float fine_fade_depth_m;   /* [0.05,100], fades from the surface to this depth */
} JceWaterCausticDetailComponent;

JCE_API JceWaterCausticDetailComponent JCE_CALL jce_scene_water_caustic_detail_default(void);
JCE_API void JCE_CALL jce_scene_set_water_caustic_detail(JceScene *scene, JceEntity entity,
    const JceWaterCausticDetailComponent *value);
JCE_API JceWaterCausticDetailComponent *JCE_CALL jce_scene_get_water_caustic_detail(
    JceScene *scene, JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_caustic_detail(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_caustic_detail(JceScene *scene, JceEntity entity);

JCE_EXTERN_C_END
#endif
