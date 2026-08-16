/* Generic scene-owned full-screen render-effect descriptor. */

#ifndef JCE_SCENE_FULLSCREEN_EFFECT_H
#define JCE_SCENE_FULLSCREEN_EFFECT_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_fullscreen_effect.h>

JCE_EXTERN_C_BEGIN

#define JCE_FULLSCREEN_EFFECT_VERSION 1u
typedef struct JceSceneFullscreenEffect {
    uint32_t struct_size;
    uint32_t version;
    bool enabled;
    bool required;
    bool use_scene_color;
    bool use_scene_depth;
    bool use_history;
    uint8_t texture_count;
    uint16_t _pad0;
    int32_t order;
    uint32_t insertion;
    uint32_t blend;
    uint32_t output_format;
    float resolution_scale;
    char shader[JCE_FULLSCREEN_EFFECT_PATH_MAX];
    char textures[JCE_FULLSCREEN_EFFECT_MAX_TEXTURES][JCE_FULLSCREEN_EFFECT_PATH_MAX];
    JceSamplerDesc samplers[JCE_FULLSCREEN_EFFECT_MAX_TEXTURES];
    float params[JCE_FULLSCREEN_EFFECT_MAX_PARAMS][4];
} JceSceneFullscreenEffect;

JCE_API JceSceneFullscreenEffect jce_scene_fullscreen_effect_default(void);
JCE_API void jce_scene_set_fullscreen_effect(JceScene *scene, JceEntity entity,
                                             const JceSceneFullscreenEffect *value);
JCE_API JceSceneFullscreenEffect *jce_scene_get_fullscreen_effect(
    JceScene *scene, JceEntity entity);
JCE_API bool jce_scene_has_fullscreen_effect(const JceScene *scene, JceEntity entity);
JCE_API void jce_scene_remove_fullscreen_effect(JceScene *scene, JceEntity entity);
JCE_API void jce_scene_each_fullscreen_effect(JceScene *scene,
                                              JceEntityCallback callback,
                                              void *userdata);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_FULLSCREEN_EFFECT_H */
