/* Camera Align/Pilot adapter. Keeps gestures atomic in the editor history. */

#include "jce_editor_scene_camera_tools.h"

#include "jce_editor_camera_tools.h"
#include "jce_editor_scene_render.h"
#include "core/jce_editor_state.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_camera.h>

#include <cmath>

namespace {

struct PilotState {
    uint32_t entity = 0;
    bool transaction_open = false;
};

PilotState s_pilot;
float s_viewport_aspect = 16.0f / 9.0f;

bool selected_camera(JceScene **out_scene, JceEntity *out_entity,
                     JceCameraComponent **out_camera)
{
    JceScene *scene = jce_state_get_scene();
    const uint32_t focused = jce_state_get_focused();
    if (!scene || focused == 0 || !jce_state_entity_exists(focused))
        return false;

    JceCameraComponent *camera =
        jce_scene_get_camera(scene, static_cast<JceEntity>(focused));
    if (!camera)
        return false;

    if (out_scene) *out_scene = scene;
    if (out_entity) *out_entity = static_cast<JceEntity>(focused);
    if (out_camera) *out_camera = camera;
    return true;
}

bool entity_camera_pose(JceScene *scene, JceEntity entity,
                        JceEditorCameraPose *out_pose)
{
    if (!scene || entity == JCE_ENTITY_INVALID || !out_pose)
        return false;

    const jce_mat4 world = jce_scene_get_world_matrix(scene, entity);
    jce_quat rotation;
    jce_m4_decompose(&world, &out_pose->position, &rotation, nullptr);
    out_pose->forward = jce_q_rotate(rotation, jce_v3(0.0f, 0.0f, -1.0f));
    out_pose->up = jce_q_rotate(rotation, jce_v3(0.0f, 1.0f, 0.0f));
    return true;
}

void apply_projection(const JceCameraComponent &camera, float aspect)
{
    const float safe_aspect = aspect > 1.0e-4f ? aspect : 1.0f;
    /* Orthographic height in world units.  Until JceCameraComponent carried
     * ortho_size there was no such field, so this reused fov_deg -- an ANGLE
     * -- as a world-unit span: a 60-degree fov became a 60-unit tall box.
     * The runtime meanwhile applied no size at all and kept
     * jce_camera_create's 800x450 default, so the editor and the shipped
     * game framed the same orthographic scene two different wrong ways.
     * With a real size authored, both now take the aspect-following path. */
    const bool authored = camera.ortho_size > 0.0f;
    const float vertical_span = authored ? camera.ortho_size
                                         : fmaxf(camera.fov_deg, 0.01f);
    jce_editor_scene_camera_set_projection(camera.ortho,
                                           camera.fov_deg,
                                           camera.near_plane,
                                           camera.far_plane,
                                           authored ? 0.0f
                                                    : vertical_span * safe_aspect,
                                           vertical_span);
}

void finish_transaction(void)
{
    if (!s_pilot.transaction_open)
        return;
    jce_state_commit_transaction();
    s_pilot.transaction_open = false;
}

bool pilot_target(JceScene **out_scene, JceEntity *out_entity,
                  JceCameraComponent **out_camera)
{
    JceScene *scene = nullptr;
    JceEntity entity = JCE_ENTITY_INVALID;
    JceCameraComponent *camera = nullptr;
    if (!selected_camera(&scene, &entity, &camera)
        || static_cast<uint32_t>(entity) != s_pilot.entity)
        return false;
    if (out_scene) *out_scene = scene;
    if (out_entity) *out_entity = entity;
    if (out_camera) *out_camera = camera;
    return true;
}

bool write_editor_pose_to_entity(JceScene *scene, JceEntity entity)
{
    JceCamera *camera = jce_editor_scene_get_camera();
    JceTransform *current = jce_scene_get_transform(scene, entity);
    if (!camera || !current)
        return false;

    JceEditorCameraPose pose{};
    pose.position = jce_camera_get_position(camera);
    pose.forward = jce_camera_get_forward(camera);
    pose.up = jce_camera_get_up(camera);

    jce_quat world_rotation;
    if (!jce_editor_camera_tools_make_rotation(pose, &world_rotation))
        return false;

    const jce_mat4 current_world = jce_scene_get_world_matrix(scene, entity);
    jce_vec3 world_scale;
    jce_m4_decompose(&current_world, nullptr, nullptr, &world_scale);
    jce_mat4 desired_world = jce_m4_from_trs(pose.position, world_rotation,
                                             jce_v3_safe_scale(world_scale));

    const JceEntity parent = jce_scene_get_parent(scene, entity);
    if (parent != JCE_ENTITY_INVALID) {
        const jce_mat4 parent_world = jce_scene_get_world_matrix(scene, parent);
        const jce_mat4 inv_parent = jce_m4_inverse(&parent_world);
        desired_world = jce_m4_multiply(&inv_parent, &desired_world);
    }

    const JcePivotComponent *pivot = jce_scene_get_pivot(scene, entity);
    if (pivot && (pivot->local_position.x != 0.0f
                  || pivot->local_position.y != 0.0f
                  || pivot->local_position.z != 0.0f)) {
        const jce_mat4 pivot_offset = jce_m4_translate(pivot->local_position);
        desired_world = jce_m4_multiply(&desired_world, &pivot_offset);
    }

    JceTransform next = *current;
    jce_m4_decompose(&desired_world, &next.position, &next.rotation, nullptr);
    next.rotation = jce_q_normalize(next.rotation);
    jce_scene_set_transform(scene, entity, &next);
    return true;
}

} /* namespace */

bool jce_editor_scene_camera_align_selected(void)
{
    JceScene *scene = nullptr;
    JceEntity entity = JCE_ENTITY_INVALID;
    JceCameraComponent *camera = nullptr;
    if (!selected_camera(&scene, &entity, &camera))
        return false;

    JceEditorCameraPose pose{};
    if (!entity_camera_pose(scene, entity, &pose))
        return false;

    float target[3];
    float yaw = 0.0f, pitch = 0.0f, distance = 0.0f;
    jce_editor_scene_camera_get_state(target, &yaw, &pitch, &distance);
    if (!std::isfinite(distance) || distance <= 1.0e-4f)
        distance = fmaxf(camera->near_plane * 10.0f, 10.0f);

    JceEditorCameraOrbit orbit{};
    if (!jce_editor_camera_tools_make_orbit(pose, distance, &orbit))
        return false;

    const float orbit_target[3] = {
        orbit.target.x, orbit.target.y, orbit.target.z,
    };
    jce_editor_scene_camera_set_state(orbit_target, orbit.yaw,
                                      orbit.pitch, orbit.distance);
    apply_projection(*camera, s_viewport_aspect);
    return true;
}

bool jce_editor_scene_camera_toggle_pilot(void)
{
    if (s_pilot.entity != 0) {
        jce_editor_scene_camera_stop_pilot();
        return false;
    }

    JceEntity entity = JCE_ENTITY_INVALID;
    if (!selected_camera(nullptr, &entity, nullptr)
        || !jce_editor_scene_camera_align_selected())
        return false;

    s_pilot.entity = static_cast<uint32_t>(entity);
    return true;
}

bool jce_editor_scene_camera_is_piloting(void)
{
    return s_pilot.entity != 0;
}

void jce_editor_scene_camera_stop_pilot(void)
{
    finish_transaction();
    s_pilot.entity = 0;
}

void jce_editor_scene_camera_update_pilot(bool navigation_active,
                                          float viewport_aspect)
{
    if (viewport_aspect > 1.0e-4f)
        s_viewport_aspect = viewport_aspect;

    if (s_pilot.entity == 0)
        return;

    if (jce_state_get_play_state() != JCE_PLAY_STOPPED) {
        jce_editor_scene_camera_stop_pilot();
        return;
    }

    JceScene *scene = nullptr;
    JceEntity entity = JCE_ENTITY_INVALID;
    JceCameraComponent *camera = nullptr;
    if (!pilot_target(&scene, &entity, &camera)) {
        jce_editor_scene_camera_stop_pilot();
        return;
    }

    apply_projection(*camera, viewport_aspect);

    if (!navigation_active) {
        finish_transaction();
        return;
    }

    if (!s_pilot.transaction_open) {
        if (!jce_state_begin_transaction("Pilot Camera"))
            return;
        s_pilot.transaction_open = true;
    }

    if (!write_editor_pose_to_entity(scene, entity))
        jce_editor_scene_camera_stop_pilot();
}
