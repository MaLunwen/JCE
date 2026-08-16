#include <jce/middleware/scene/jce_scene_camera.h>

#include <math.h>
#include <string.h>

typedef struct JceSceneCameraScan {
    JceSceneCameraPose pose;
    uint32_t primary_count;
    bool invalid_pose;
} JceSceneCameraScan;

static bool scene_camera_vec3_finite(jce_vec3 value)
{
    return isfinite(value.x) && isfinite(value.y) && isfinite(value.z);
}

static bool scene_camera_build_pose(JceScene *scene, JceEntity entity,
                                    const JceCameraComponent *component,
                                    JceSceneCameraPose *out_pose)
{
    const float epsilon = 1.0e-6f;
    jce_mat4 world;
    jce_vec3 raw_up;
    jce_vec3 forward;
    jce_vec3 right;
    jce_vec3 up;

    if (!scene || !component || !out_pose ||
        !jce_scene_has_transform(scene, entity))
        return false;
    if (!isfinite(component->near_plane) ||
        !isfinite(component->far_plane) ||
        component->near_plane <= 0.0f ||
        component->far_plane <= component->near_plane)
        return false;
    if (!component->ortho && (!isfinite(component->fov_deg) ||
        component->fov_deg <= 0.0f || component->fov_deg >= 180.0f))
        return false;

    world = jce_scene_get_world_matrix(scene, entity);
    raw_up = jce_v3(world.raw[1][0], world.raw[1][1], world.raw[1][2]);
    forward = jce_v3(-world.raw[2][0], -world.raw[2][1],
                     -world.raw[2][2]);
    if (!scene_camera_vec3_finite(raw_up) ||
        !scene_camera_vec3_finite(forward) ||
        jce_v3_len(raw_up) <= epsilon || jce_v3_len(forward) <= epsilon)
        return false;

    forward = jce_v3_normalize(forward);
    right = jce_v3_cross(forward, raw_up);
    if (!scene_camera_vec3_finite(right) || jce_v3_len(right) <= epsilon)
        return false;
    right = jce_v3_normalize(right);
    up = jce_v3_normalize(jce_v3_cross(right, forward));

    memset(out_pose, 0, sizeof(*out_pose));
    out_pose->entity = entity;
    out_pose->position = jce_v3(world.raw[3][0], world.raw[3][1],
                                world.raw[3][2]);
    out_pose->right = right;
    out_pose->up = up;
    out_pose->forward = forward;
    out_pose->fov_deg = component->fov_deg;
    out_pose->near_plane = component->near_plane;
    out_pose->far_plane = component->far_plane;
    out_pose->orthographic = component->ortho;
    return scene_camera_vec3_finite(out_pose->position);
}

static void scene_camera_scan_primary(JceScene *scene, JceEntity entity,
                                      void *user_data)
{
    JceSceneCameraScan *scan = (JceSceneCameraScan *)user_data;
    JceCameraComponent *component;

    if (!scan || !scene ||
        !jce_scene_component_enabled(scene, entity, JCE_COMP_FLAG_CAMERA))
        return;
    component = jce_scene_get_camera(scene, entity);
    if (!component || !component->is_primary)
        return;

    scan->primary_count++;
    if (scan->primary_count == 1u &&
        !scene_camera_build_pose(scene, entity, component, &scan->pose))
        scan->invalid_pose = true;
}

JceSceneCameraResolveResult jce_scene_camera_resolve_primary(
    JceScene *scene, JceSceneCameraPose *out_pose)
{
    JceSceneCameraScan scan;

    if (!scene || !out_pose)
        return JCE_SCENE_CAMERA_RESOLVE_INVALID_ARGUMENT;
    memset(&scan, 0, sizeof(scan));
    jce_scene_each_camera(scene, scene_camera_scan_primary, &scan);
    if (scan.primary_count == 0u)
        return JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND;
    if (scan.primary_count > 1u)
        return JCE_SCENE_CAMERA_RESOLVE_AMBIGUOUS;
    if (scan.invalid_pose)
        return JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE;
    *out_pose = scan.pose;
    return JCE_SCENE_CAMERA_RESOLVE_OK;
}

JceSceneCameraResolveResult jce_scene_camera_apply_primary(
    JceScene *scene, JceCamera *camera, JceSceneCameraPose *out_pose)
{
    JceSceneCameraPose pose;
    JceSceneCameraResolveResult result;

    if (!camera)
        return JCE_SCENE_CAMERA_RESOLVE_INVALID_ARGUMENT;
    result = jce_scene_camera_resolve_primary(scene, &pose);
    if (result != JCE_SCENE_CAMERA_RESOLVE_OK)
        return result;
    if (!jce_camera_set_pose(camera, pose.position, pose.forward, pose.up))
        return JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE;
    jce_camera_set_mode(camera, pose.orthographic
        ? JCE_CAMERA_ORTHO : JCE_CAMERA_PERSPECTIVE);
    jce_camera_set_fov(camera, pose.fov_deg);
    jce_camera_set_near_far(camera, pose.near_plane, pose.far_plane);
    if (out_pose)
        *out_pose = pose;
    return JCE_SCENE_CAMERA_RESOLVE_OK;
}
