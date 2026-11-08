#ifndef JCE_SCENE_WATER_SPECTRUM_H
#define JCE_SCENE_WATER_SPECTRUM_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_water_field.h>

JCE_EXTERN_C_BEGIN

#define JCE_WATER_SPECTRUM_COMPONENT_VERSION 1u
#define JCE_WATER_SPECTRUM_FILTER_NORMALS 1u
/* Optional FFT authoring extension. Absence/disable keeps legacy Water
 * fetch selection. Wind, seed, fetch and swell remain owned by Water. */
typedef struct JceWaterSpectrumComponent {
    uint32_t struct_size;
    uint32_t version;
    uint32_t model; /* JCE_WATER_FFT_SPECTRUM_* */
    float depth;   /* TMA water depth, metres, > 0 */
    float directional_blend; /* 0 isotropic, 1 wind-aligned */
    uint32_t shading_flags; /* JCE_WATER_SPECTRUM_FILTER_NORMALS; 0 keeps vertex normals */
} JceWaterSpectrumComponent;

JCE_API JceWaterSpectrumComponent JCE_CALL jce_scene_water_spectrum_default(void);
JCE_API void JCE_CALL jce_scene_set_water_spectrum(JceScene *scene, JceEntity entity,
    const JceWaterSpectrumComponent *value);
JCE_API JceWaterSpectrumComponent *JCE_CALL jce_scene_get_water_spectrum(
    JceScene *scene, JceEntity entity);
JCE_API bool JCE_CALL jce_scene_has_water_spectrum(const JceScene *scene, JceEntity entity);
JCE_API void JCE_CALL jce_scene_remove_water_spectrum(JceScene *scene, JceEntity entity);

/* Resolves the existing Water and optional spectrum on the scene's shared
 * clock. Both rendering and physics use this entry. NULL on invalid input or
 * failed rebuild; an existing field remains intact on failure. */
JCE_API JceWaterField *JCE_CALL jce_scene_acquire_water_field(JceScene *scene,
    JceEntity entity);

JCE_EXTERN_C_END
#endif
