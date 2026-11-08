#ifndef JCE_SCENE_WATER_NORMAL_DETAIL_H
#define JCE_SCENE_WATER_NORMAL_DETAIL_H

#include <jce/middleware/scene/jce_scene.h>
JCE_EXTERN_C_BEGIN

/** Current WaterNormalDetail authoring schema. */
#define JCE_WATER_NORMAL_DETAIL_COMPONENT_VERSION 1u

/** Optional FFT-water micro-normal authoring. Absence or component disable keeps
 * the legacy static detail. This does not change the Water ABI or simulation.
 * Frequencies are independent noise-coordinate scales in inverse metres; their
 * order is preserved. Strength zero disables this component's micro-normal.
 * Set/get/remove follow the scene's main-thread mutation contract. */
typedef struct JceWaterNormalDetailComponent {
    uint32_t struct_size;   /**< At least sizeof this version; stored size is normalized. */
    uint32_t version;       /**< JCE_WATER_NORMAL_DETAIL_COMPONENT_VERSION. */
    float frequency_low;   /**< [0.1,64] inverse metres; default 6. */
    float frequency_high;  /**< [0.1,64] inverse metres; default 16; may be below low. */
    float strength;        /**< [0,1] slope amplitude; default 0.14. */
} JceWaterNormalDetailComponent;

/** Return an independent initialized value; no allocation or scene access. */
JCE_API JceWaterNormalDetailComponent JCE_CALL jce_scene_water_normal_detail_default(void);
/** Copy valid data into scene-owned storage. Null/invalid input or an invalid
 * entity is a no-op and preserves an existing value. Main thread only. */
JCE_API void JCE_CALL jce_scene_set_water_normal_detail(JceScene *scene, JceEntity entity,
    const JceWaterNormalDetailComponent *value);
/** Return a borrowed scene-owned value, or NULL if absent/invalid. Main thread
 * only; use the setter for edits and do not retain across scene mutations. */
JCE_API JceWaterNormalDetailComponent *JCE_CALL jce_scene_get_water_normal_detail(
    JceScene *scene, JceEntity entity);
/** Query presence, independently of component enable state. Main thread only;
 * a null scene or invalid entity returns false. */
JCE_API bool JCE_CALL jce_scene_has_water_normal_detail(const JceScene *scene, JceEntity entity);
/** Remove only this extension. Null/invalid/absent values are no-ops. Main
 * thread only; the Water body and shared simulation remain owned by the scene. */
JCE_API void JCE_CALL jce_scene_remove_water_normal_detail(JceScene *scene, JceEntity entity);

JCE_EXTERN_C_END
#endif
