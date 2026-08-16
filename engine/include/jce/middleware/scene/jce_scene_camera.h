/*
 * jce_scene_camera.h  Scene-authored camera to render-camera binding.
 */

#ifndef JCE_SCENE_CAMERA_H
#define JCE_SCENE_CAMERA_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_camera.h>

JCE_EXTERN_C_BEGIN

typedef enum JceSceneCameraResolveResult {
    JCE_SCENE_CAMERA_RESOLVE_OK = 0,
    JCE_SCENE_CAMERA_RESOLVE_INVALID_ARGUMENT,
    JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND,
    JCE_SCENE_CAMERA_RESOLVE_AMBIGUOUS,
    JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE
} JceSceneCameraResolveResult;

typedef struct JceSceneCameraPose {
    JceEntity entity;
    jce_vec3 position;
    jce_vec3 right;
    jce_vec3 up;
    jce_vec3 forward;
    float fov_deg;
    float near_plane;
    float far_plane;
    bool orthographic;
} JceSceneCameraPose;

/* Resolve the single enabled primary Camera component in world space.
 * Ambiguous authoring is rejected instead of depending on ECS iteration order. */
JCE_API JceSceneCameraResolveResult jce_scene_camera_resolve_primary(
    JceScene *scene, JceSceneCameraPose *out_pose);

/* Resolve and atomically apply the primary scene camera to a render camera.
 * out_pose is optional and receives the exact applied values. */
JCE_API JceSceneCameraResolveResult jce_scene_camera_apply_primary(
    JceScene *scene, JceCamera *camera, JceSceneCameraPose *out_pose);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_CAMERA_H */
