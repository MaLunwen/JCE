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
    out_pose->culling_mask = component->culling_mask;
    out_pose->clear_mode = component->clear_mode;
    out_pose->fov_deg = component->fov_deg;
    out_pose->near_plane = component->near_plane;
    out_pose->far_plane = component->far_plane;
    out_pose->orthographic = component->ortho;
    out_pose->ortho_size   = component->ortho_size;
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

bool jce_scene_camera_apply_pose(JceCamera *camera,
                                 const JceSceneCameraPose *pose)
{
    if (!camera || !pose)
        return false;
    if (!jce_camera_set_pose(camera, pose->position, pose->forward, pose->up))
        return false;
    jce_camera_set_mode(camera, pose->orthographic
        ? JCE_CAMERA_ORTHO : JCE_CAMERA_PERSPECTIVE);
    if (pose->orthographic && pose->ortho_size > 0.0f)
        jce_camera_set_ortho_height(camera, pose->ortho_size);
    jce_camera_set_fov(camera, pose->fov_deg);
    jce_camera_set_near_far(camera, pose->near_plane, pose->far_plane);
    return true;
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
    if (!jce_scene_camera_apply_pose(camera, &pose))
        return JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE;
    if (out_pose)
        *out_pose = pose;
    return JCE_SCENE_CAMERA_RESOLVE_OK;
}

/* See the header for why DEPTH_ONLY and NOTHING answer the same as COLOR
 * today.  Only the three authored non-default modes turn the sky off; anything
 * else -- including a hand-edited clearMode out of range -- keeps the
 * behaviour every existing scene already has. */
bool jce_scene_camera_clear_draws_skybox(uint8_t clear_mode)
{
    switch ((JceCameraClearMode)clear_mode) {
    case JCE_CAMERA_CLEAR_COLOR:
    case JCE_CAMERA_CLEAR_DEPTH_ONLY:
    case JCE_CAMERA_CLEAR_NOTHING:
        return false;
    default:
        return true;
    }
}

/* The OTHER half of the clear question.  Kept beside the sky answer on
 * purpose: two switches over the same enum in two files is how they start
 * disagreeing. */
void jce_scene_camera_clear_keeps(uint8_t clear_mode,
                                  bool *out_keep_color,
                                  bool *out_keep_depth)
{
    bool keep_color = false;
    bool keep_depth = false;

    switch ((JceCameraClearMode)clear_mode) {
    case JCE_CAMERA_CLEAR_DEPTH_ONLY:
        keep_color = true;
        break;
    case JCE_CAMERA_CLEAR_NOTHING:
        keep_color = true;
        keep_depth = true;
        break;
    default:
        /* SKYBOX, COLOR, and any out-of-range value: the full clear every
         * existing scene already gets. */
        break;
    }

    if (out_keep_color) *out_keep_color = keep_color;
    if (out_keep_depth) *out_keep_depth = keep_depth;
}

/* ── Camera stacking ──────────────────────────────────────────────── */

typedef struct JceSceneCameraStackScan {
    JceSceneCameraPose *out;
    uint8_t             max;
    uint8_t             count;
    bool                ambiguous;
    bool                invalid_pose;
    /* Which stack indices have been claimed, so a duplicate is caught without
     * an O(n^2) rescan and without depending on visit order. */
    uint32_t            seen_mask;
} JceSceneCameraStackScan;

static void scene_camera_scan_overlay(JceScene *scene, JceEntity entity,
                                      void *user_data)
{
    JceSceneCameraStackScan *scan = (JceSceneCameraStackScan *)user_data;
    JceCameraComponent *component;
    JceSceneCameraPose pose;
    uint8_t idx;

    if (!scan || !scene ||
        !jce_scene_component_enabled(scene, entity, JCE_COMP_FLAG_CAMERA))
        return;
    component = jce_scene_get_camera(scene, entity);
    if (!component || component->is_primary || component->stack_index == 0u)
        return;

    idx = component->stack_index;
    if (idx < 32u) {
        if (scan->seen_mask & (1u << idx)) {
            scan->ambiguous = true;
            return;
        }
        scan->seen_mask |= (1u << idx);
    }
    if (scan->count >= scan->max)
        return;                      /* past the cap: counted by absence */
    if (!scene_camera_build_pose(scene, entity, component, &pose)) {
        scan->invalid_pose = true;
        return;
    }
    pose.stack_index = idx;
    scan->out[scan->count++] = pose;
}

JceSceneCameraResolveResult jce_scene_camera_resolve_stack(
    JceScene *scene, JceSceneCameraPose *out_overlays, uint8_t max_overlays,
    uint8_t *out_count)
{
    JceSceneCameraStackScan scan;
    uint8_t i, j;

    if (!scene || !out_overlays || !out_count || max_overlays == 0u)
        return JCE_SCENE_CAMERA_RESOLVE_INVALID_ARGUMENT;

    memset(&scan, 0, sizeof(scan));
    scan.out = out_overlays;
    scan.max = max_overlays;
    *out_count = 0u;

    jce_scene_each_camera(scene, scene_camera_scan_overlay, &scan);
    if (scan.ambiguous)
        return JCE_SCENE_CAMERA_RESOLVE_AMBIGUOUS;
    if (scan.invalid_pose)
        return JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE;

    /* Sort by stack_index.  Insertion sort over at most a handful of entries,
     * and the point is not speed: without it the draw order is ECS visit
     * order, which is the one thing this file refuses to depend on. */
    for (i = 1u; i < scan.count; i++) {
        JceSceneCameraPose k = scan.out[i];
        for (j = i; j > 0u && scan.out[j - 1u].stack_index > k.stack_index; j--)
            scan.out[j] = scan.out[j - 1u];
        scan.out[j] = k;
    }

    *out_count = scan.count;
    return JCE_SCENE_CAMERA_RESOLVE_OK;
}
