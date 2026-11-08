#ifndef JCE_EDITOR_DEFAULT_CAMERA_H
#define JCE_EDITOR_DEFAULT_CAMERA_H
#include <jce/middleware/scene/jce_scene.h>

/* Used only by the default-scene builders. Adding another authored Camera
   retains the general component-default policy (non-primary). */
static inline bool jce_editor_seed_default_camera(JceScene *scene, JceEntity entity, bool is_2d)
{
    if (!scene || !entity) return false;
    JceCameraComponent *camera = jce_scene_get_camera(scene, entity);
    JceTransform *transform = jce_scene_get_transform(scene, entity);
    if (!camera || !transform) return false;
    camera->is_primary = true;
    camera->ortho = is_2d;
    if (is_2d) {
        camera->ortho_size = 5.0f;
        transform->position = jce_v3(0.0f, 0.0f, 10.0f);
    }
    return true;
}
#endif
