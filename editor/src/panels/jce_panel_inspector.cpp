/*
 * jce_panel_inspector.cpp  Inspector panel (entity properties).
 *
 * Reads from / writes to the ECS scene directly via jce_scene_*.
 */

#include "ui/jce_editor_colors.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_presets.h"
#include "core/jce_editor_state_internal.h"
#include "core/jce_project_settings.h"
#include "core/jce_reflect.h"
#include "core/jce_editor_prefab_overrides.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_model_loader_assimp.h"

#include <jce/tools/jce_imgui.hpp>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include <algorithm>
#include <cmath>

/* Per-entity euler cache shared with the scene-view gizmo. Implemented in
 * jce_scene_view_helpers.cpp; declared inline here to avoid pulling in the
 * full scene-view internal header. */
extern "C++" {
bool jce_editor_get_cached_euler_deg(uint32_t entity_id, jce_quat current_q, float out_deg[3]);
void jce_editor_set_cached_euler_deg(uint32_t entity_id, jce_quat q, const float deg[3]);
}

extern "C" {
#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_pbr_material.h>
}

/* ── Component clipboard for Copy/Paste Values ────────────────────── */

static struct {
    uint64_t flag;
    char     data[4096];
    size_t   data_size;
} s_comp_clipboard = { 0, {0}, 0 };

static void *comp_get_ptr_and_size(JceScene *scene, JceEntity e,
                                   uint64_t flag, size_t *out_size);

void *jce_inspector_comp_blob(JceScene *scene, JceEntity e,
                              uint64_t flag, size_t *out_size)
{
    return comp_get_ptr_and_size(scene, e, flag, out_size);
}

static void *comp_get_ptr_and_size(JceScene *scene, JceEntity e,
                                   uint64_t flag, size_t *out_size)
{
    if (!scene) return NULL;
    switch (flag) {
    case JCE_COMP_FLAG_TRANSFORM:
        *out_size = sizeof(JceTransform);
        return jce_scene_get_transform(scene, e);
    case JCE_COMP_FLAG_MESH_RENDERER:
        *out_size = sizeof(JceMeshRenderer);
        return jce_scene_get_mesh_renderer(scene, e);
    case JCE_COMP_FLAG_CAMERA:
        *out_size = sizeof(JceCameraComponent);
        return jce_scene_get_camera(scene, e);
    case JCE_COMP_FLAG_RIGIDBODY:
        *out_size = sizeof(JceRigidBodyComponent);
        return jce_scene_get_rigidbody(scene, e);
    case JCE_COMP_FLAG_BOX_COLLIDER:
        *out_size = sizeof(JceBoxColliderComponent);
        return jce_scene_get_box_collider(scene, e);
    case JCE_COMP_FLAG_SPHERE_COLLIDER:
        *out_size = sizeof(JceSphereColliderComponent);
        return jce_scene_get_sphere_collider(scene, e);
    case JCE_COMP_FLAG_AUDIO_SOURCE:
        *out_size = sizeof(JceAudioSourceComponent);
        return jce_scene_get_audio_source(scene, e);
    case JCE_COMP_FLAG_RIGIDBODY_2D:
        *out_size = sizeof(JceRigidBody2DComponent);
        return jce_scene_get_rigidbody2d(scene, e);
    case JCE_COMP_FLAG_PARTICLE_EMITTER:
        *out_size = sizeof(JceParticleEmitterComponent);
        return jce_scene_get_particle_emitter(scene, e);
    case JCE_COMP_FLAG_BEHAVIOR_TREE:
        *out_size = sizeof(JceBehaviorTree);
        return jce_scene_get_behavior_tree(scene, e);
    case JCE_COMP_FLAG_LOD_GROUP:
        *out_size = sizeof(JceLodGroupComponent);
        return jce_scene_get_lod_group(scene, e);
    case JCE_COMP_FLAG_VIRTUAL_CAMERA:
        *out_size = sizeof(JceVirtualCameraComponent);
        return jce_scene_get_virtual_camera(scene, e);
    case JCE_COMP_FLAG_TRIGGER_VOLUME:
        *out_size = sizeof(JceTriggerVolumeComponent);
        return jce_scene_get_trigger_volume(scene, e);
    case JCE_COMP_FLAG_CAPSULE_COLLIDER:
        *out_size = sizeof(JceCapsuleColliderComponent);
        return jce_scene_get_capsule_collider(scene, e);
    case JCE_COMP_FLAG_MESH_COLLIDER:
        *out_size = sizeof(JceMeshColliderComponent);
        return jce_scene_get_mesh_collider(scene, e);
    case JCE_COMP_FLAG_COLLIDER_2D:
        *out_size = sizeof(JceCollider2DComponent);
        return jce_scene_get_collider2d(scene, e);
    case JCE_COMP_FLAG_TRAIL_RENDERER:
        *out_size = sizeof(JceTrailRendererComponent);
        return jce_scene_get_trail_renderer(scene, e);
    case JCE_COMP_FLAG_LINE_RENDERER:
        *out_size = sizeof(JceLineRendererComponent);
        return jce_scene_get_line_renderer(scene, e);
    case JCE_COMP_FLAG_REFLECTION_PROBE:
        *out_size = sizeof(JceReflectionProbeComponent);
        return jce_scene_get_reflection_probe(scene, e);
    case JCE_COMP_FLAG_DECAL:
        *out_size = sizeof(JceDecalComponent);
        return jce_scene_get_decal(scene, e);
    case JCE_COMP_FLAG_LIGHT_PROBE_GROUP:
        *out_size = sizeof(JceLightProbeGroupComponent);
        return jce_scene_get_light_probe_group(scene, e);
    case JCE_COMP_FLAG_AUDIO_LISTENER:
        *out_size = sizeof(JceAudioListenerComponent);
        return jce_scene_get_audio_listener(scene, e);
    case JCE_COMP_FLAG_AUDIO_REVERB_ZONE:
        *out_size = sizeof(JceAudioReverbZoneComponent);
        return jce_scene_get_audio_reverb_zone(scene, e);
    case JCE_COMP_FLAG_AUDIO_OCCLUSION:
        *out_size = sizeof(JceAudioOcclusionComponent);
        return jce_scene_get_audio_occlusion(scene, e);
    case JCE_COMP_FLAG_SPAWN_MANAGER:
        *out_size = sizeof(JceSpawnManagerComponent);
        return jce_scene_get_spawn_manager(scene, e);
    case JCE_COMP_FLAG_WEAPON:
        *out_size = sizeof(JceWeaponComponent);
        return jce_scene_get_weapon(scene, e);
    case JCE_COMP_FLAG_SAVE_POINT:
        *out_size = sizeof(JceSavePointComponent);
        return jce_scene_get_save_point(scene, e);
    case JCE_COMP_FLAG_WHEEL_COLLIDER:
        *out_size = sizeof(JceWheelColliderComponent);
        return jce_scene_get_wheel_collider(scene, e);
    case JCE_COMP_FLAG_CONSTANT_FORCE:
        *out_size = sizeof(JceConstantForceComponent);
        return jce_scene_get_constant_force(scene, e);
    case JCE_COMP_FLAG_CONFIGURABLE_JOINT:
        *out_size = sizeof(JceConfigurableJointComponent);
        return jce_scene_get_configurable_joint(scene, e);
    case JCE_COMP_FLAG_JOINT_2D:
        *out_size = sizeof(JceJoint2DComponent);
        return jce_scene_get_joint2d(scene, e);
    case JCE_COMP_FLAG_BILLBOARD_RENDERER:
        *out_size = sizeof(JceBillboardRendererComponent);
        return jce_scene_get_billboard_renderer(scene, e);
    case JCE_COMP_FLAG_CANVAS:
        *out_size = sizeof(JceCanvasComponent);
        return jce_scene_get_canvas(scene, e);
    case JCE_COMP_FLAG_CANVAS_GROUP:
        *out_size = sizeof(JceCanvasGroupComponent);
        return jce_scene_get_canvas_group(scene, e);
    case JCE_COMP_FLAG_LAYOUT_GROUP:
        *out_size = sizeof(JceLayoutGroupComponent);
        return jce_scene_get_layout_group(scene, e);
    case JCE_COMP_FLAG_UI_IMAGE:
        *out_size = sizeof(JceUIImageComponent);
        return jce_scene_get_ui_image(scene, e);
    case JCE_COMP_FLAG_UI_TEXT:
        *out_size = sizeof(JceUITextComponent);
        return jce_scene_get_ui_text(scene, e);
    case JCE_COMP_FLAG_UI_BUTTON:
        *out_size = sizeof(JceUIButtonComponent);
        return jce_scene_get_ui_button(scene, e);
    case JCE_COMP_FLAG_UI_TOGGLE:
        *out_size = sizeof(JceUIToggleComponent);
        return jce_scene_get_ui_toggle(scene, e);
    case JCE_COMP_FLAG_UI_SLIDER:
        *out_size = sizeof(JceUISliderComponent);
        return jce_scene_get_ui_slider(scene, e);
    case JCE_COMP_FLAG_UI_DROPDOWN:
        *out_size = sizeof(JceUIDropdownComponent);
        return jce_scene_get_ui_dropdown(scene, e);
    case JCE_COMP_FLAG_UI_INPUT_FIELD:
        *out_size = sizeof(JceUIInputFieldComponent);
        return jce_scene_get_ui_input_field(scene, e);
    case JCE_COMP_FLAG_UI_RAW_IMAGE:
        *out_size = sizeof(JceUIRawImageComponent);
        return jce_scene_get_ui_raw_image(scene, e);
    case JCE_COMP_FLAG_UI_OUTLINE_EFFECT:
        *out_size = sizeof(JceUIOutlineEffect);
        return jce_scene_get_ui_outline_effect(scene, e);
    case JCE_COMP_FLAG_UI_SHADOW_EFFECT:
        *out_size = sizeof(JceUIShadowEffect);
        return jce_scene_get_ui_shadow_effect(scene, e);
    case JCE_COMP_FLAG_POINT_LIGHT_2D:
        *out_size = sizeof(JcePointLight2DComponent);
        return jce_scene_get_point_light_2d(scene, e);
    case JCE_COMP_FLAG_SPOT_LIGHT_2D:
        *out_size = sizeof(JceSpotLight2DComponent);
        return jce_scene_get_spot_light_2d(scene, e);
    case JCE_COMP_FLAG_GLOBAL_LIGHT_2D:
        *out_size = sizeof(JceGlobalLight2DComponent);
        return jce_scene_get_global_light_2d(scene, e);
    default:
        *out_size = 0;
        return NULL;
    }
}

/* ── Tag colors (display data) ────────────────────────────────────── */

static const ImVec4 s_tag_colors[JCE_TAG_COLOR_COUNT] = {
    ImVec4(0, 0, 0, 0),
    JCE_COLOR_TAG_RED,
    JCE_COLOR_TAG_ORANGE,
    JCE_COLOR_TAG_YELLOW,
    JCE_COLOR_TAG_GREEN,
    JCE_COLOR_TAG_BLUE,
    JCE_COLOR_TAG_PURPLE,
    JCE_COLOR_TAG_GRAY,
};

/* ── Inspector state ──────────────────────────────────────────────── */

static struct {
    char name_buf[JCE_MAX_ENTITY_NAME];
    char tag_buf[JCE_MAX_TAG_STRING];
    bool needs_sync;
    bool initialized;
    bool delete_requested;
    uint32_t delete_entity_ids[JCE_MAX_SELECTED];
    int delete_entity_count;
} s_insp;

static void ensure_init(void)
{
    if (s_insp.initialized) return;
    memset(&s_insp, 0, sizeof(s_insp));
    s_insp.needs_sync  = true;
    s_insp.initialized = true;
}

void jce_editor_inspector_request_sync(void)
{
    s_insp.needs_sync = true;
}

void jce_editor_inspector_request_delete_confirm(uint32_t entity_id)
{
    ensure_init();
    if (entity_id == 0) return;
    s_insp.delete_entity_ids[0] = entity_id;
    s_insp.delete_entity_count = 1;
    s_insp.delete_requested = true;
}

void jce_editor_inspector_request_delete_confirm_many(const uint32_t *entity_ids,
                                                      int entity_count)
{
    ensure_init();
    if (!entity_ids || entity_count <= 0) return;

    if (entity_count > JCE_MAX_SELECTED)
        entity_count = JCE_MAX_SELECTED;

    int write_count = 0;
    for (int i = 0; i < entity_count; i++) {
        uint32_t id = entity_ids[i];
        if (id == 0) continue;

        bool duplicate = false;
        for (int j = 0; j < write_count; j++) {
            if (s_insp.delete_entity_ids[j] == id) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        s_insp.delete_entity_ids[write_count++] = id;
    }

    if (write_count <= 0) return;

    s_insp.delete_entity_count = write_count;
    s_insp.delete_requested = true;
}

bool jce_editor_inspector_delete_dialog_open(void)
{
    return s_insp.delete_requested;
}

/* ── Undo tracking helpers ─────────────────────────────────────────── */

static bool s_insp_batch_open = false;

/* Deferred component removal — applied at end of frame to avoid
 * mutating the entity while the inspector is still iterating its
 * components (which would invalidate flecs pointers and crash the
 * post-edit history snapshot). */
static struct {
    uint32_t entity_id;
    uint64_t flag;
    bool     pending;
} s_pending_remove = { 0, 0, false };

/* Preset save modal — name buffer is shared across sections. */
static char s_preset_save_buf[64] = { 0 };

/* Deferred component reorder — applied at end of frame. dir = -1 (up),
 * +1 (down), or 0 (drag set absolute target index). */
static struct {
    uint32_t entity_id;
    uint64_t src_flag;
    int      dir;        /* -1, +1, or 0 (use target_index) */
    size_t   target_index;
    bool     pending;
} s_pending_move = { 0, 0, 0, 0, false };

/* Drag-reorder transient state: which header is being dragged, and over
 * which header the cursor currently hovers. Cleared on mouse release. */
static struct {
    uint32_t entity_id;
    uint64_t src_flag;
    uint64_t hover_flag;
    bool     active;
} s_drag = { 0, 0, 0, false };

static void insp_track_edit(void)
{
    if (ImGui::IsItemActivated() && !s_insp_batch_open) {
        jce_state_begin_batch_edit();
        s_insp_batch_open = true;
    }
    if (ImGui::IsItemDeactivated() && s_insp_batch_open) {
        jce_state_end_batch_edit();
        s_insp_batch_open = false;
    }
}

static void insp_undo_bool(bool *value)
{
    bool now = *value;
    *value = !now;
    jce_state_begin_batch_edit();
    *value = now;
    jce_state_end_batch_edit();
}

static void insp_undo_int(int *value, int prev)
{
    int now = *value;
    *value = prev;
    jce_state_begin_batch_edit();
    *value = now;
    jce_state_end_batch_edit();
}

/* ── Field-level "Reset" right-click context menu ─────────────────────
 * Wraps the most-recently-drawn ImGui item with a Unity-style right-
 * click "Reset" menu that restores the field to a known default. */
#define INSP_RESET_CTX(POPUP_ID, ...)                                       \
    do {                                                                    \
        if (ImGui::BeginPopupContextItem(POPUP_ID)) {                       \
            if (ImGui::MenuItem(jce_editor_i18n("inspector.resetField"))) { \
                jce_state_begin_batch_edit();                               \
                __VA_ARGS__;                                                \
                jce_state_end_batch_edit();                                 \
            }                                                               \
            ImGui::EndPopup();                                              \
        }                                                                   \
    } while (0)

/* ── Vec3 control (colored XYZ drag floats) ───────────────────────── */

static void draw_vec3_control(const char *label,
                              float *values,
                              float speed = 0.1f,
                              float reset_value = 0.0f)
{
    ImGui::PushID(label);

    float line_h = ImGui::GetFrameHeight();
    ImVec2 btn_size = ImVec2(line_h + 3.0f, line_h);
    float width = (ImGui::CalcItemWidth() - btn_size.x * 3.0f -
                   ImGui::GetStyle().ItemInnerSpacing.x * 2.0f) / 3.0f;

    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_X);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("X", btn_size)) {
        jce_state_begin_batch_edit();
        values[0] = reset_value;
        jce_state_end_batch_edit();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##X", &values[0], speed);
    insp_track_edit();
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Y);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.9f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
    if (ImGui::Button("Y", btn_size)) {
        jce_state_begin_batch_edit();
        values[1] = reset_value;
        jce_state_end_batch_edit();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Y", &values[1], speed);
    insp_track_edit();
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Z);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.2f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.1f, 0.8f, 1.0f));
    if (ImGui::Button("Z", btn_size)) {
        jce_state_begin_batch_edit();
        values[2] = reset_value;
        jce_state_end_batch_edit();
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Z", &values[2], speed);
    insp_track_edit();
    ImGui::PopItemWidth();

    ImGui::PopID();
}

/* ── Asset drag-drop target for path fields ───────────────────────── */

static void accept_asset_drop(char *buf, size_t buf_size)
{
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
            const char *path = (const char *)payload->Data;
            snprintf(buf, buf_size, "%s", path);
        }
        ImGui::EndDragDropTarget();
    }
}

static bool is_mesh_ext(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return false;
    static const char *const exts[] = {
        ".fbx", ".FBX", ".glb", ".GLB", ".gltf", ".GLTF",
        ".obj", ".OBJ", ".mesh", ".MESH", ".dae", ".DAE", NULL
    };
    for (int i = 0; exts[i]; i++)
        if (strcmp(ext, exts[i]) == 0) return true;
    return false;
}

static void accept_mesh_drop_with_material(JceMeshRenderer *mr)
{
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
            const char *path = (const char *)payload->Data;
            snprintf(mr->mesh_path, sizeof(mr->mesh_path), "%s", path);
            mr->mesh_shape = 0;

            if (is_mesh_ext(path)) {
                JceEditorMaterialInfo mat = {};
                if (jce_editor_model_extract_material(path, &mat)) {
                    if (mat.albedo_tex[0])
                        snprintf(mr->albedo_tex, sizeof(mr->albedo_tex), "%s", mat.albedo_tex);
                    if (mat.mr_tex[0])
                        snprintf(mr->mr_tex, sizeof(mr->mr_tex), "%s", mat.mr_tex);
                    if (mat.normal_tex[0])
                        snprintf(mr->normal_tex, sizeof(mr->normal_tex), "%s", mat.normal_tex);
                    if (mat.ao_tex[0])
                        snprintf(mr->ao_tex, sizeof(mr->ao_tex), "%s", mat.ao_tex);
                    if (mat.emissive_tex[0])
                        snprintf(mr->emissive_tex, sizeof(mr->emissive_tex), "%s", mat.emissive_tex);

                    mr->base_color[0] = mat.base_color[0];
                    mr->base_color[1] = mat.base_color[1];
                    mr->base_color[2] = mat.base_color[2];
                    mr->base_color[3] = mat.base_color[3];
                    mr->metallic       = mat.metallic;
                    mr->roughness      = mat.roughness;
                    mr->emissive[0]    = mat.emissive[0];
                    mr->emissive[1]    = mat.emissive[1];
                    mr->emissive[2]    = mat.emissive[2];
                    mr->normal_scale   = mat.normal_scale;
                    mr->ao_strength    = mat.ao_strength;
                    mr->alpha_mode     = mat.alpha_mode;
                    mr->alpha_cutoff   = mat.alpha_cutoff;
                    mr->double_sided   = mat.double_sided;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

/* ── Per-component draw helpers ───────────────────────────────────── */

static void draw_comp_transform(uint32_t entity_id, JceTransform *t)
{
    float pos[3] = {t->position.x, t->position.y, t->position.z};
    float scl[3] = {t->scale.x,    t->scale.y,    t->scale.z};
    float rot[3];
    if (!jce_editor_get_cached_euler_deg(entity_id, t->rotation, rot))
        editor_q_to_euler_deg(t->rotation, rot);

    ImGui::Text("%s", jce_editor_i18n("transform.position"));
    ImGui::SameLine(80);
    draw_vec3_control("Position", pos);
    if (pos[0] != t->position.x || pos[1] != t->position.y || pos[2] != t->position.z) {
        t->position.x = pos[0]; t->position.y = pos[1]; t->position.z = pos[2];
    }

    ImGui::Text("%s", jce_editor_i18n("transform.rotation"));
    ImGui::SameLine(80);
    float rot_in[3] = {rot[0], rot[1], rot[2]};
    draw_vec3_control("Rotation", rot, 1.0f);
    for (int a = 0; a < 3; a++) {
        rot[a] = fmodf(rot[a], 360.0f);
        if (rot[a] < 0.0f) rot[a] += 360.0f;
    }
    if (rot[0] != rot_in[0] || rot[1] != rot_in[1] || rot[2] != rot_in[2]) {
        t->rotation = editor_q_from_euler_deg(rot);
        jce_editor_set_cached_euler_deg(entity_id, t->rotation, rot);
    }

    ImGui::Text("%s", jce_editor_i18n("transform.scale"));
    ImGui::SameLine(80);
    draw_vec3_control("Scale", scl, 0.01f, 1.0f);
    if (scl[0] != t->scale.x || scl[1] != t->scale.y || scl[2] != t->scale.z) {
        t->scale.x = scl[0]; t->scale.y = scl[1]; t->scale.z = scl[2];
    }
}

static void draw_comp_light(JceScene *scene, JceEntity e, uint64_t flags)
{
    char lbl[256];

    int light_type = -1;
    if (flags & JCE_COMP_FLAG_DIR_LIGHT)        light_type = 0;
    else if (flags & JCE_COMP_FLAG_POINT_LIGHT) light_type = 1;
    else if (flags & JCE_COMP_FLAG_SPOT_LIGHT)  light_type = 2;
    if (light_type < 0) return;

    /* Color + Intensity (common to all light types) — read/write whichever exists. */
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    if (light_type == 0) {
        JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    } else if (light_type == 1) {
        JcePointLight *l = jce_scene_get_point_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    } else {
        JceSpotLight *l = jce_scene_get_spot_light(scene, e);
        color[0] = l->color.x; color[1] = l->color.y; color[2] = l->color.z;
        intensity = l->intensity;
    }

    snprintf(lbl, sizeof(lbl), "%s###Color", jce_editor_i18n("light.color"));
    if (ImGui::ColorEdit3(lbl, color)) {
        if (light_type == 0)      { JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);   l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
        else if (light_type == 1) { JcePointLight       *l = jce_scene_get_point_light(scene, e); l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
        else                      { JceSpotLight        *l = jce_scene_get_spot_light(scene, e);  l->color.x=color[0]; l->color.y=color[1]; l->color.z=color[2]; }
    }
    INSP_RESET_CTX("##rst_lightColor",
        if (light_type == 0)      { JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);   l->color = { 1.0f, 1.0f, 1.0f }; }
        else if (light_type == 1) { JcePointLight       *l = jce_scene_get_point_light(scene, e); l->color = { 1.0f, 1.0f, 1.0f }; }
        else                      { JceSpotLight        *l = jce_scene_get_spot_light(scene, e);  l->color = { 1.0f, 1.0f, 1.0f }; });
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Intensity", jce_editor_i18n("light.intensity"));
    if (ImGui::DragFloat(lbl, &intensity, 0.1f, 0.0f, 100.0f)) {
        if (light_type == 0)      { jce_scene_get_dir_light(scene, e)->intensity   = intensity; }
        else if (light_type == 1) { jce_scene_get_point_light(scene, e)->intensity = intensity; }
        else                      { jce_scene_get_spot_light(scene, e)->intensity  = intensity; }
    }
    INSP_RESET_CTX("##rst_lightIntensity",
        if (light_type == 0)      { jce_scene_get_dir_light(scene, e)->intensity   = 1.0f; }
        else if (light_type == 1) { jce_scene_get_point_light(scene, e)->intensity = 1.0f; }
        else                      { jce_scene_get_spot_light(scene, e)->intensity  = 1.0f; });
    insp_track_edit();

    /* Type combo. */
    const char *light_types[] = {
        jce_editor_i18n("light.directional"),
        jce_editor_i18n("light.point"),
        jce_editor_i18n("light.spot")
    };
    int new_type = light_type;
    snprintf(lbl, sizeof(lbl), "%s###Type", jce_editor_i18n("light.type"));
    if (ImGui::Combo(lbl, &new_type, light_types, 3) && new_type != light_type) {
        /* Carry over color + intensity. */
        jce_state_begin_batch_edit();
        if (light_type == 0) jce_scene_remove_dir_light(scene, e);
        else if (light_type == 1) jce_scene_remove_point_light(scene, e);
        else jce_scene_remove_spot_light(scene, e);

        if (new_type == 0) {
            JceDirectionalLight l = {};
            l.direction.x = 0; l.direction.y = -1; l.direction.z = 0;
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.casts_shadow = false;
            jce_scene_set_dir_light(scene, e, &l);
        } else if (new_type == 1) {
            JcePointLight l = {};
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.radius = 10.0f;
            jce_scene_set_point_light(scene, e, &l);
        } else {
            JceSpotLight l = {};
            l.direction.x = 0; l.direction.y = -1; l.direction.z = 0;
            l.color.x = color[0]; l.color.y = color[1]; l.color.z = color[2];
            l.intensity = intensity;
            l.radius = 10.0f;
            l.inner_cone_cos = cosf(25.0f * JCE_DEG2RAD);
            l.outer_cone_cos = cosf(35.0f * JCE_DEG2RAD);
            jce_scene_set_spot_light(scene, e, &l);
        }
        jce_state_end_batch_edit();
        light_type = new_type;
    }

    /* Type-specific fields. */
    if (light_type == 1 || light_type == 2) {
        float radius = (light_type == 1)
            ? jce_scene_get_point_light(scene, e)->radius
            : jce_scene_get_spot_light(scene, e)->radius;
        snprintf(lbl, sizeof(lbl), "%s###Radius", jce_editor_i18n("collider.radius"));
        if (ImGui::DragFloat(lbl, &radius, 0.1f, 0.01f, 1000.0f)) {
            if (light_type == 1) jce_scene_get_point_light(scene, e)->radius = radius;
            else                 jce_scene_get_spot_light(scene, e)->radius  = radius;
        }
        insp_track_edit();
    }

    if (light_type == 2) {
        JceSpotLight *l = jce_scene_get_spot_light(scene, e);
        float inner_deg = acosf(l->inner_cone_cos) * JCE_RAD2DEG;
        float outer_deg = acosf(l->outer_cone_cos) * JCE_RAD2DEG;
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.innerCone", "InnerCone"), &inner_deg, 0.5f, 0.0f, 89.0f))
            l->inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
        insp_track_edit();
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.light.outerCone", "OuterCone"), &outer_deg, 0.5f, 0.0f, 90.0f))
            l->outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
        insp_track_edit();
        /* Clamp: outer must be >= inner (i.e. outer cos <= inner cos). */
        if (l->outer_cone_cos > l->inner_cone_cos)
            l->outer_cone_cos = l->inner_cone_cos;
    }

    if (light_type == 0) {
        JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.light.castsShadow", "CastsShadow"), &l->casts_shadow))
            insp_undo_bool(&l->casts_shadow);
    }
}

static void draw_comp_camera(JceCameraComponent *cam)
{
    static const JceReflectType *t = jce_reflect_find("Camera");
    if (t) {
        jce_reflect_draw(t, cam);
        return;
    }
    /* Fallback: legacy hand-written drawer if reflection registry is empty. */
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s###FOV", jce_editor_i18n("camera.fov"));
    ImGui::DragFloat(lbl, &cam->fov_deg, 1.0f, 1.0f, 179.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Near", jce_editor_i18n("camera.nearClip"));
    ImGui::DragFloat(lbl, &cam->near_plane, 0.01f, 0.001f, 100.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Far", jce_editor_i18n("camera.farClip"));
    ImGui::DragFloat(lbl, &cam->far_plane, 1.0f, 1.0f, 100000.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Orthographic", jce_editor_i18n("camera.orthographic"));
    if (ImGui::Checkbox(lbl, &cam->ortho))
        insp_undo_bool(&cam->ortho);

    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.camera.primary", "CamPrimary"), &cam->is_primary))
        insp_undo_bool(&cam->is_primary);
}

static bool is_mat_json(const char *path)
{
    if (!path) return false;
    size_t len = strlen(path);
    return (len >= 9 && strcmp(path + len - 9, ".mat.json") == 0);
}

static void load_material_into_renderer(JceMeshRenderer *mr)
{
    if (!is_mat_json(mr->material_path)) return;

    JcePbrMaterial mat;
    char tex_paths[5][256] = {};
    if (!jce_pbr_material_load_json(mr->material_path, &mat, tex_paths))
        return;

    mr->base_color[0] = mat.base_color_factor[0];
    mr->base_color[1] = mat.base_color_factor[1];
    mr->base_color[2] = mat.base_color_factor[2];
    mr->base_color[3] = mat.base_color_factor[3];
    mr->metallic       = mat.metallic_factor;
    mr->roughness      = mat.roughness_factor;
    mr->emissive[0]    = mat.emissive_factor[0];
    mr->emissive[1]    = mat.emissive_factor[1];
    mr->emissive[2]    = mat.emissive_factor[2];
    mr->normal_scale   = mat.normal_scale;
    mr->ao_strength    = mat.ao_strength;
    mr->alpha_mode     = (int)mat.alpha_mode;
    mr->alpha_cutoff   = mat.alpha_cutoff;
    mr->double_sided   = mat.double_sided;

    if (tex_paths[0][0]) snprintf(mr->albedo_tex,   sizeof(mr->albedo_tex),   "%s", tex_paths[0]);
    if (tex_paths[1][0]) snprintf(mr->mr_tex,       sizeof(mr->mr_tex),       "%s", tex_paths[1]);
    if (tex_paths[2][0]) snprintf(mr->normal_tex,   sizeof(mr->normal_tex),   "%s", tex_paths[2]);
    if (tex_paths[3][0]) snprintf(mr->ao_tex,       sizeof(mr->ao_tex),       "%s", tex_paths[3]);
    if (tex_paths[4][0]) snprintf(mr->emissive_tex, sizeof(mr->emissive_tex), "%s", tex_paths[4]);
}

static void accept_material_drop(JceMeshRenderer *mr)
{
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
            const char *path = (const char *)payload->Data;
            jce_state_begin_batch_edit();
            snprintf(mr->material_path, sizeof(mr->material_path), "%s", path);
            load_material_into_renderer(mr);
            jce_state_end_batch_edit();
        }
        ImGui::EndDragDropTarget();
    }
}

static void draw_comp_mesh_renderer(JceMeshRenderer *mr)
{
    char lbl[256];

    const char *shape_names[] = {
        jce_editor_i18n("menu.gameObject.createCube"),
        jce_editor_i18n("menu.gameObject.createSphere"),
        jce_editor_i18n("menu.gameObject.createPlane"),
        jce_editor_i18n("inspector.shape.capsule"),
        jce_editor_i18n("menu.gameObject.createCylinder")
    };
    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("inspector.shape"));
    ImGui::SameLine();
    int prev_shape = mr->mesh_shape;
    ImGui::Combo("##mesh_shape", &mr->mesh_shape, shape_names, 5);
    if (mr->mesh_shape != prev_shape)
        insp_undo_int(&mr->mesh_shape, prev_shape);

    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.mesh"));
    ImGui::SameLine();
    ImGui::InputText("##mesh_path", mr->mesh_path, sizeof(mr->mesh_path));
    insp_track_edit();
    accept_mesh_drop_with_material(mr);

    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.materials"));
    ImGui::SameLine();
    ImGui::InputText("##mat_path", mr->material_path, sizeof(mr->material_path));
    if (ImGui::IsItemDeactivatedAfterEdit() && is_mat_json(mr->material_path)) {
        jce_state_begin_batch_edit();
        load_material_into_renderer(mr);
        jce_state_end_batch_edit();
    }
    insp_track_edit();
    accept_material_drop(mr);
    if (is_mat_json(mr->material_path)) {
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("codeViewer.reload"))) {
            jce_state_begin_batch_edit();
            load_material_into_renderer(mr);
            jce_state_end_batch_edit();
        }
    }

    if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.pbrMaterial"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit4(jce_editor_i18n("inspector.baseColor"), mr->base_color);
        INSP_RESET_CTX("##rst_baseColor",
                       mr->base_color[0] = 1.0f; mr->base_color[1] = 1.0f;
                       mr->base_color[2] = 1.0f; mr->base_color[3] = 1.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("viewer.metallic"), &mr->metallic, 0.01f, 0.0f, 1.0f);
        INSP_RESET_CTX("##rst_metallic", mr->metallic = 0.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("viewer.roughness"), &mr->roughness, 0.01f, 0.0f, 1.0f);
        INSP_RESET_CTX("##rst_roughness", mr->roughness = 0.5f);
        insp_track_edit();
        ImGui::ColorEdit3(jce_editor_i18n("inspector.emissive"), mr->emissive);
        INSP_RESET_CTX("##rst_emissive",
                       mr->emissive[0] = 0.0f; mr->emissive[1] = 0.0f; mr->emissive[2] = 0.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.normalScale"), &mr->normal_scale, 0.01f, 0.0f, 4.0f);
        INSP_RESET_CTX("##rst_normalScale", mr->normal_scale = 1.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.aoStrength"), &mr->ao_strength, 0.01f, 0.0f, 2.0f);
        INSP_RESET_CTX("##rst_aoStrength", mr->ao_strength = 1.0f);
        insp_track_edit();
        const char *alpha_modes[] = {
            jce_editor_i18n("inspector.alphaMode.opaque"),
            jce_editor_i18n("inspector.alphaMode.mask"),
            jce_editor_i18n("inspector.alphaMode.blend")
        };
        int prev_alpha = mr->alpha_mode;
        ImGui::Combo(jce_editor_i18n("inspector.alphaMode"), &mr->alpha_mode, alpha_modes, 3);
        if (mr->alpha_mode != prev_alpha)
            insp_undo_int(&mr->alpha_mode, prev_alpha);
        if (mr->alpha_mode == 1) {
            ImGui::DragFloat(jce_editor_i18n("inspector.alphaCutoff"), &mr->alpha_cutoff, 0.01f, 0.0f, 1.0f);
            insp_track_edit();
        }
        if (ImGui::Checkbox(jce_editor_i18n("inspector.doubleSided"), &mr->double_sided))
            insp_undo_bool(&mr->double_sided);
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.textures"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText(jce_editor_i18n("inspector.texture.albedo"), mr->albedo_tex, 128);
        insp_track_edit();
        accept_asset_drop(mr->albedo_tex, 128);
        ImGui::InputText(jce_editor_i18n("inspector.texture.metalRough"), mr->mr_tex, 128);
        insp_track_edit();
        accept_asset_drop(mr->mr_tex, 128);
        ImGui::InputText(jce_editor_i18n("inspector.texture.normal"), mr->normal_tex, 128);
        insp_track_edit();
        accept_asset_drop(mr->normal_tex, 128);
        ImGui::InputText(jce_editor_i18n("inspector.texture.ao"), mr->ao_tex, 128);
        insp_track_edit();
        accept_asset_drop(mr->ao_tex, 128);
        snprintf(lbl, sizeof(lbl), "%s###tex", jce_editor_i18n("inspector.texture.emissive"));
        ImGui::InputText(lbl, mr->emissive_tex, 128);
        insp_track_edit();
        accept_asset_drop(mr->emissive_tex, 128);
        ImGui::TreePop();
    }
}

static void draw_comp_sprite_renderer(JceSpriteRendererComponent *sr)
{
    ImGui::InputText(jce_editor_i18n("spriteRenderer.sprite"), sr->sprite_path, 128);
    insp_track_edit();
    accept_asset_drop(sr->sprite_path, 128);
    ImGui::ColorEdit4(jce_editor_i18n("spriteRenderer.color"), sr->color);
    INSP_RESET_CTX("##rst_spriteColor",
                   sr->color[0] = 1.0f; sr->color[1] = 1.0f;
                   sr->color[2] = 1.0f; sr->color[3] = 1.0f);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("spriteRenderer.flipX"), &sr->flip_x))
        insp_undo_bool(&sr->flip_x);
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n("spriteRenderer.flipY"), &sr->flip_y))
        insp_undo_bool(&sr->flip_y);
    ImGui::DragInt(jce_editor_i18n("spriteRenderer.orderInLayer"), &sr->sorting_order);
    insp_track_edit();
}

static void draw_comp_animator(JceAnimatorComponent *anim)
{
    if (anim->speed <= 0.0f) anim->speed = 1.0f;

    ImGui::InputText(jce_editor_i18n("timeline.clip"), anim->clip_name, 64);
    insp_track_edit();
    accept_asset_drop(anim->clip_name, 64);
    ImGui::DragFloat(jce_editor_i18n("timeline.speed"), &anim->speed, 0.01f, 0.01f, 10.0f);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("timeline.loop"), &anim->loop))
        insp_undo_bool(&anim->loop);
    if (ImGui::Button(anim->playing
                      ? jce_editor_i18n("toolbar.stop")
                      : jce_editor_i18n("toolbar.play")))
        anim->playing = !anim->playing;
}

static void draw_comp_skeletal_animator(JceSkeletalAnimatorComponent *skel)
{
    char lbl[256];

    if (skel->speed <= 0.0f) skel->speed = 1.0f;

    ImGui::InputText(jce_editor_i18n("inspector.skeleton"), skel->skeleton_path, 128);
    insp_track_edit();
    accept_asset_drop(skel->skeleton_path, 128);

    if (skel->skeleton_path[0] && skel->clip_count == 0) {
        JceModel *mdl = jce_editor_scene_get_model(skel->skeleton_path, 0);
        if (mdl) {
            uint32_t n = jce_model_anim_count(mdl);
            if (n > 8) n = 8;
            skel->clip_count = (int)n;
            for (uint32_t ci = 0; ci < n; ++ci) {
                JceAnimClip *clip = jce_model_get_anim(mdl, ci);
                const char *name = clip ? jce_anim_clip_name(clip) : "clip";
                snprintf(skel->clip_names[ci], sizeof(skel->clip_names[ci]),
                         "%s", name ? name : "clip");
            }
        }
    }

    snprintf(lbl, sizeof(lbl), "%s###skelSpeed", jce_editor_i18n("timeline.speed"));
    ImGui::DragFloat(lbl, &skel->speed, 0.01f, 0.01f, 10.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###skelLoop", jce_editor_i18n("timeline.loop"));
    if (ImGui::Checkbox(lbl, &skel->loop))
        insp_undo_bool(&skel->loop);

    int clip_count = skel->clip_count;
    if (clip_count < 0) clip_count = 0;
    if (clip_count > 8) clip_count = 8;
    if (skel->active_clip < 0) skel->active_clip = 0;
    if (clip_count > 0 && skel->active_clip >= clip_count)
        skel->active_clip = clip_count - 1;

    if (clip_count > 0) {
        int prev_clip = skel->active_clip;
        ImGui::Combo(jce_editor_i18n("inspector.activeClip"), &skel->active_clip,
            [](void *data, int idx) -> const char* {
                auto *sa = (JceSkeletalAnimatorComponent *)data;
                return sa->clip_names[idx]; },
            skel,
            clip_count);
        if (skel->active_clip != prev_clip)
            insp_undo_int(&skel->active_clip, prev_clip);
    }

    if (skel->skeleton_path[0]) {
        JceAnimPlayer *pl = jce_editor_scene_get_anim_player(skel->skeleton_path, 0);
        if (pl) {
            float t = jce_anim_player_get_time(pl);
            JceModel *mdl = jce_editor_scene_get_model(skel->skeleton_path, 0);
            float dur = 1.0f;
            if (mdl) {
                JceAnimClip *clip = jce_model_get_anim(mdl, (uint32_t)skel->active_clip);
                if (clip) dur = jce_anim_clip_duration(clip);
            }
            float frac = (dur > 0.0f) ? (t / dur) : 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            snprintf(lbl, sizeof(lbl), "%.2fs / %.2fs", t, dur);
            ImGui::ProgressBar(frac, ImVec2(-1, 0), lbl);
        }
    }

    snprintf(lbl, sizeof(lbl), "%s###skelPlay",
             skel->playing ? jce_editor_i18n("toolbar.stop")
                           : jce_editor_i18n("toolbar.play"));
    if (ImGui::Button(lbl))
        skel->playing = !skel->playing;
}

static void draw_comp_rigidbody(JceRigidBodyComponent *rb)
{
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s###rbMass", jce_editor_i18n("rigidbody.mass"));
    ImGui::DragFloat(lbl, &rb->mass, 0.1f, 0.0f, 10000.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###rbDrag", jce_editor_i18n("rigidbody.drag"));
    ImGui::DragFloat(lbl, &rb->drag, 0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###rbAngularDrag", jce_editor_i18n("rigidbody.angularDrag"));
    ImGui::DragFloat(lbl, &rb->angular_drag, 0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###rbUseGravity", jce_editor_i18n("rigidbody.useGravity"));
    if (ImGui::Checkbox(lbl, &rb->use_gravity))
        insp_undo_bool(&rb->use_gravity);
    snprintf(lbl, sizeof(lbl), "%s###rbIsKinematic", jce_editor_i18n("rigidbody.isKinematic"));
    if (ImGui::Checkbox(lbl, &rb->is_kinematic))
        insp_undo_bool(&rb->is_kinematic);
}

static void draw_comp_box_collider(JceBoxColliderComponent *bc)
{
    char lbl[256];
    ImGui::Text("%s", jce_editor_i18n("collider.center"));
    ImGui::SameLine(80);
    draw_vec3_control("BoxCenter", bc->center);
    ImGui::Text("%s", jce_editor_i18n("collider.size"));
    ImGui::SameLine(80);
    draw_vec3_control("BoxSize", bc->size, 0.01f, 1.0f);
    snprintf(lbl, sizeof(lbl), "%s###box", jce_editor_i18n("collider.isTrigger"));
    if (ImGui::Checkbox(lbl, &bc->is_trigger))
        insp_undo_bool(&bc->is_trigger);
}

static void draw_comp_sphere_collider(JceSphereColliderComponent *sc)
{
    char lbl[256];
    ImGui::Text("%s", jce_editor_i18n("collider.center"));
    ImGui::SameLine(80);
    draw_vec3_control("SphereCenter", sc->center);
    ImGui::DragFloat(jce_editor_i18n("collider.radius"), &sc->radius, 0.01f, 0.001f, 1000.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###sphere", jce_editor_i18n("collider.isTrigger"));
    if (ImGui::Checkbox(lbl, &sc->is_trigger))
        insp_undo_bool(&sc->is_trigger);
}

static void draw_comp_character_controller(JceCharacterControllerComponent *cc)
{
    char lbl[256];
    ImGui::DragFloat(jce_editor_i18n("collider.height"), &cc->height, 0.1f, 0.1f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###cc", jce_editor_i18n("collider.radius"));
    ImGui::DragFloat(lbl, &cc->radius, 0.01f, 0.01f, 50.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.stepOffset"), &cc->step_offset, 0.01f, 0.0f, 10.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.slopeLimit"), &cc->slope_limit, 1.0f, 0.0f, 90.0f);
    insp_track_edit();
}

static void draw_comp_audio_source(JceAudioSourceComponent *as)
{
    char lbl[256];
    ImGui::InputText(jce_editor_i18n("audioSource.clip"), as->clip_path, 128);
    insp_track_edit();
    accept_asset_drop(as->clip_path, 128);
    ImGui::DragFloat(jce_editor_i18n("audioSource.volume"), &as->volume, 0.01f, 0.0f, 1.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("audioSource.pitch"), &as->pitch, 0.01f, 0.01f, 3.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.spatialBlend"), &as->spatial_blend, 0.01f, 0.0f, 1.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###audio", jce_editor_i18n("audioSource.loop"));
    if (ImGui::Checkbox(lbl, &as->loop))
        insp_undo_bool(&as->loop);
    if (ImGui::Checkbox(jce_editor_i18n("audioSource.playOnAwake"), &as->play_on_awake))
        insp_undo_bool(&as->play_on_awake);
}

static void draw_comp_script(JceScriptComponent *scr)
{
    ImGui::InputText("##script_path", scr->script_path, 128);
    insp_track_edit();
    accept_asset_drop(scr->script_path, 128);
}

static void draw_comp_skybox(JceSkyboxComponent *sky)
{
    ImGui::InputText(jce_editor_i18n("skybox.hdrPath"), sky->hdr_path, 256);
    insp_track_edit();
    accept_asset_drop(sky->hdr_path, 256);
    ImGui::DragFloat(jce_editor_i18n("skybox.rotation"), &sky->rotation, 1.0f, 0.0f, 360.0f, "%.1f deg");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("skybox.exposure"), &sky->exposure, 0.01f, 0.01f, 10.0f, "%.2f");
    insp_track_edit();
    if (sky->exposure <= 0.0f) sky->exposure = 1.0f;
    if (ImGui::Checkbox(jce_editor_i18n("skybox.useAsIbl"), &sky->use_as_ibl))
        insp_undo_bool(&sky->use_as_ibl);
}

static void draw_comp_sprite_animator(JceSpriteAnimatorComponent *sa)
{
    ImGui::InputText(jce_editor_i18n("spriteAnimator.sheetPath"), sa->sheet_path, 128);
    insp_track_edit();
    accept_asset_drop(sa->sheet_path, 128);
    ImGui::InputText(jce_editor_i18n("spriteAnimator.atlasPath"), sa->atlas_path, 128);
    insp_track_edit();
    accept_asset_drop(sa->atlas_path, 128);
    ImGui::DragInt(jce_editor_i18n("spriteAnimator.frameWidth"), &sa->frame_width, 1, 1, 4096);
    insp_track_edit();
    ImGui::DragInt(jce_editor_i18n("spriteAnimator.frameHeight"), &sa->frame_height, 1, 1, 4096);
    insp_track_edit();
    ImGui::InputText(jce_editor_i18n("spriteAnimator.animation"), sa->current_anim, 64);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("spriteAnimator.speed"), &sa->speed, 0.01f, 0.0f, 10.0f, "%.2f");
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("spriteAnimator.loop"), &sa->loop))
        insp_undo_bool(&sa->loop);
    if (ImGui::Checkbox(jce_editor_i18n("spriteAnimator.playing"), &sa->playing))
        insp_undo_bool(&sa->playing);
}

static void draw_comp_constraint(JceConstraintComponent *con)
{
    const char *constraint_types[] = { "Point2Point", "Hinge", "Slider", "6DOF" };
    int prev_type = con->constraint_type;
    if (ImGui::Combo(jce_editor_i18n("constraint.type"), &con->constraint_type, constraint_types, 4))
        insp_undo_int(&con->constraint_type, prev_type);
    ImGui::DragFloat3(jce_editor_i18n("constraint.pivotA"), con->pivot_a, 0.1f);
    insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n("constraint.pivotB"), con->pivot_b, 0.1f);
    insp_track_edit();
    if (con->constraint_type == 1 || con->constraint_type == 2) {
        ImGui::DragFloat3(jce_editor_i18n("constraint.axis"), con->axis, 0.1f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("constraint.lowerLimit"), &con->lower_limit, 0.1f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("constraint.upperLimit"), &con->upper_limit, 0.1f);
        insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n("constraint.disableCollision"), &con->disable_collision))
        insp_undo_bool(&con->disable_collision);
}

static void draw_comp_terrain(JceTerrainComponent *tc)
{
    if (!tc) return;
    ImGui::InputText(jce_editor_i18n("inspector.terrain.path"), tc->terrain_path, sizeof tc->terrain_path);
    insp_track_edit();
    accept_asset_drop(tc->terrain_path, sizeof tc->terrain_path);
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.terrain.dropHint"));

    if (ImGui::Checkbox(jce_editor_i18n("inspector.terrain.visible"), &tc->visible))
        insp_undo_bool(&tc->visible);

    ImGui::ColorEdit3(jce_editor_i18n("inspector.terrain.tint"), tc->tint);
    insp_track_edit();

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.terrain.splatHeader"));
    if (ImGui::Checkbox(jce_editor_i18n("inspector.terrain.splatEnable"), &tc->splat_enabled))
        insp_undo_bool(&tc->splat_enabled);
    if (tc->tile_scale <= 0.0f) tc->tile_scale = 10.0f;
    ImGui::DragFloat(jce_editor_i18n("inspector.terrain.layerTile"), &tc->tile_scale, 0.1f, 0.1f, 256.0f, "%.2f");
    insp_track_edit();

    for (int i = 0; i < 4; i++) {
        char label[32];
        snprintf(label, sizeof label, jce_editor_i18n("inspector.terrain.layerFmt"), i);
        ImGui::PushID(i);
        ImGui::InputText(label, tc->layer_albedo_path[i],
                         sizeof tc->layer_albedo_path[i]);
        insp_track_edit();
        accept_asset_drop(tc->layer_albedo_path[i],
                          sizeof tc->layer_albedo_path[i]);
        ImGui::PopID();
    }
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.terrain.splatChannels"));

    /* Show summary if a terrain file is bound. Using load_file is heavy;
     * for the inspector we just print the path and let the Terrain panel
     * handle authoring. */
    if (tc->terrain_path[0]) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", jce_editor_i18n("inspector.terrain.useTerrainPanel"));
    } else {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                           jce_editor_i18n("inspector.terrain.notBound"));
    }
}

static void draw_comp_lod_group(JceLodGroupComponent *lg)
{
    if (!lg) return;
    if (lg->level_count < 0) lg->level_count = 0;
    if (lg->level_count > JCE_LOD_COMP_MAX_LEVELS) lg->level_count = JCE_LOD_COMP_MAX_LEVELS;
    int lc = lg->level_count;
    if (ImGui::SliderInt(jce_editor_i18n_id("inspector.lod.levelCount", "lod"), &lc, 0, JCE_LOD_COMP_MAX_LEVELS)) {
        lg->level_count = lc;
        insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.lod.hysteresis", "lod"), &lg->hysteresis, 0.01f, 0.0f, 0.5f, "%.2f");
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lod.cullWhenTooFar", "lod"), &lg->cull_when_too_far))
        insp_undo_bool(&lg->cull_when_too_far);
    ImGui::Separator();
    for (int i = 0; i < lg->level_count; ++i) {
        ImGui::PushID(i);
        char hdr[32];
        snprintf(hdr, sizeof hdr, "LOD %d", i);
        ImGui::TextUnformatted(hdr);
        ImGui::DragFloat(jce_editor_i18n("inspector.lod.lodDistance"), &lg->distances[i], 0.5f, 0.0f, 100000.0f, "%.1f");
        insp_track_edit();
        ImGui::InputText(jce_editor_i18n("inspector.lod.meshOverride"), lg->level_mesh_paths[i],
                         sizeof lg->level_mesh_paths[i]);
        insp_track_edit();
        accept_asset_drop(lg->level_mesh_paths[i], sizeof lg->level_mesh_paths[i]);
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::TextDisabled(jce_editor_i18n("inspector.lod.emptyMeshNote"));
}

static void draw_comp_virtual_camera(JceVirtualCameraComponent *vc)
{
    if (!vc) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.vcam.name", "vcam"), vc->vcam_name, sizeof vc->vcam_name);
    insp_track_edit();
    int prio = vc->priority;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.vcam.priority", "vcam"), &prio, 1, -1000, 1000)) {
        vc->priority = prio; insp_track_edit();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("inspector.vcam.solo", "vcam"))) {
        /* Bump priority above any reasonable other VCam, mark active. */
        vc->priority = 9999;
        vc->active   = true;
        insp_track_edit();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(jce_editor_i18n("inspector.vcam.soloTooltip"));
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.vcam.active", "vcam"), &vc->active))
        insp_undo_bool(&vc->active);

    static const char *track_modes[] = {
        "None", "Follow", "Look At", "Follow + Look At"
    };
    int tm = vc->track_mode; if (tm < 0 || tm > 3) tm = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.vcam.trackMode", "vcam"), &tm, track_modes, 4)) {
        vc->track_mode = tm; insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vcam.fov", "vcam"), &vc->fov_deg, 0.5f, 1.0f, 179.0f, "%.1f");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vcam.damping", "vcam"), &vc->damping, 0.01f, 0.0f, 1.0f, "%.2f");
    insp_track_edit();

    ImGui::Separator();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.vcam.position", "vcam"), vc->position, 0.1f);
    insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.vcam.lookAt", "vcam"), vc->look_at, 0.1f);
    insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.vcam.followOffset", "vcam"), vc->follow_offset, 0.1f);
    insp_track_edit();

    ImGui::Separator();
    int ft = (int)vc->follow_target;
    if (ImGui::InputInt(jce_editor_i18n_id("inspector.vcam.followTargetEntity", "vcam"), &ft)) {
        vc->follow_target = (uint64_t)(ft < 0 ? 0 : ft);
        insp_track_edit();
    }
    int lt = (int)vc->look_at_target;
    if (ImGui::InputInt(jce_editor_i18n_id("inspector.vcam.lookAtTargetEntity", "vcam"), &lt)) {
        vc->look_at_target = (uint64_t)(lt < 0 ? 0 : lt);
        insp_track_edit();
    }
    ImGui::TextDisabled(jce_editor_i18n("inspector.vcam.targetNote"));
    ImGui::Separator();
    ImGui::TextDisabled(jce_editor_i18n("inspector.vcam.playModeNote"));
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.vcam.priorityNote"));
}

static void draw_comp_trigger_volume(JceTriggerVolumeComponent *tv)
{
    if (!tv) return;
    static const char *shapes[] = { "AABB", "Sphere", "OBB" };
    int sh = tv->shape; if (sh < 0 || sh > 2) sh = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.trig.shape", "trig"), &sh, shapes, 3)) {
        tv->shape = sh; insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.enabled", "trig"), &tv->enabled))
        insp_undo_bool(&tv->enabled);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trig.fireStayEvents", "trig"), &tv->fire_stay))
        insp_undo_bool(&tv->fire_stay);
    ImGui::InputText(jce_editor_i18n_id("inspector.trig.tag", "trig"), tv->tag, sizeof tv->tag);
    insp_track_edit();

    ImGui::Separator();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.center", "trig"), tv->center, 0.1f);
    insp_track_edit();
    if (tv->shape == 1) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.trig.radius", "trig"), &tv->half_extents[0], 0.05f, 0.0f, 10000.0f, "%.2f");
        insp_track_edit();
    } else {
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.halfExtents", "trig"), tv->half_extents, 0.05f, 0.0f, 10000.0f, "%.2f");
        insp_track_edit();
    }
    if (tv->shape == 2) {
        ImGui::Separator();
        ImGui::TextUnformatted(jce_editor_i18n("inspector.trig.obbAxes"));
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.axisX", "trig"), tv->axis_x, 0.01f);
        insp_track_edit();
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.axisY", "trig"), tv->axis_y, 0.01f);
        insp_track_edit();
        ImGui::DragFloat3(jce_editor_i18n_id("inspector.trig.axisZ", "trig"), tv->axis_z, 0.01f);
        insp_track_edit();
    }
}

static void draw_comp_capsule_collider(JceCapsuleColliderComponent *cc)
{
    if (!cc) return;
    static const char *axes[] = { "X", "Y", "Z" };
    int ax = cc->axis; if (ax < 0 || ax > 2) ax = 1;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.capcol.axis", "capcol"), &ax, axes, 3)) { cc->axis = ax; insp_track_edit(); }
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.capcol.center", "capcol"), cc->center, 0.05f); insp_track_edit();
    ImGui::DragFloat (jce_editor_i18n_id("inspector.capcol.radius", "capcol"), &cc->radius, 0.01f, 0.0f, 10000.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat (jce_editor_i18n_id("inspector.capcol.height", "capcol"), &cc->height, 0.01f, 0.0f, 10000.0f, "%.3f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.capcol.isTrigger", "capcol"), &cc->is_trigger))
        insp_undo_bool(&cc->is_trigger);
}

static void draw_comp_mesh_collider(JceMeshColliderComponent *mc)
{
    if (!mc) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.meshcol.mesh", "meshcol"), mc->mesh_path, sizeof mc->mesh_path);
    insp_track_edit();
    accept_asset_drop(mc->mesh_path, sizeof mc->mesh_path);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.meshcol.convex", "meshcol"), &mc->convex))
        insp_undo_bool(&mc->convex);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.meshcol.isTrigger", "meshcol"), &mc->is_trigger))
        insp_undo_bool(&mc->is_trigger);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.meshcol.friction", "meshcol"),    &mc->friction,    0.01f, 0.0f, 10.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.meshcol.restitution", "meshcol"), &mc->restitution, 0.01f, 0.0f, 1.0f,  "%.2f"); insp_track_edit();
    if (mc->is_trigger || !mc->convex)
        ImGui::TextDisabled(jce_editor_i18n("inspector.meshcol.convexNote"));
}

static void draw_comp_collider2d(JceCollider2DComponent *cd)
{
    if (!cd) return;
    static const char *shapes[] = { "Box", "Circle", "Capsule", "Edge", "Polygon" };
    int sh = cd->shape; if (sh < 0 || sh > 4) sh = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.c2d.shape", "c2d"), &sh, shapes, 5)) { cd->shape = sh; insp_track_edit(); }
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.c2d.offset", "c2d"), cd->offset, 0.05f); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.c2d.isTrigger", "c2d"), &cd->is_trigger))
        insp_undo_bool(&cd->is_trigger);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.c2d.friction", "c2d"),    &cd->friction,    0.01f, 0.0f, 10.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.c2d.restitution", "c2d"), &cd->restitution, 0.01f, 0.0f, 1.0f,  "%.2f"); insp_track_edit();
    ImGui::Separator();
    switch (cd->shape) {
    case JCE_COLLIDER_2D_BOX:
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.c2d.size", "c2d"), cd->size, 0.05f, 0.0f, 10000.0f, "%.3f");
        insp_track_edit();
        break;
    case JCE_COLLIDER_2D_CIRCLE:
        ImGui::DragFloat(jce_editor_i18n_id("inspector.c2d.radius", "c2d"), &cd->radius, 0.01f, 0.0f, 10000.0f, "%.3f");
        insp_track_edit();
        break;
    case JCE_COLLIDER_2D_CAPSULE: {
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.c2d.size", "c2d"), cd->size, 0.05f, 0.0f, 10000.0f, "%.3f");
        insp_track_edit();
        static const char *dirs[] = { "Vertical", "Horizontal" };
        int dir = cd->capsule_direction; if (dir < 0 || dir > 1) dir = 0;
        if (ImGui::Combo(jce_editor_i18n_id("inspector.c2d.direction", "c2d"), &dir, dirs, 2)) { cd->capsule_direction = dir; insp_track_edit(); }
        break;
    }
    case JCE_COLLIDER_2D_EDGE:
    case JCE_COLLIDER_2D_POLYGON: {
        int n = cd->point_count;
        if (ImGui::SliderInt(jce_editor_i18n_id("inspector.c2d.pointCount", "c2d"), &n, 0, JCE_COLLIDER_2D_MAX_POINTS)) {
            cd->point_count = n; insp_track_edit();
        }
        for (int i = 0; i < cd->point_count; ++i) {
            ImGui::PushID(i);
            char lbl[16]; snprintf(lbl, sizeof lbl, jce_editor_i18n_id("inspector.c2d.pD", "c2d"), i);
            ImGui::DragFloat2(lbl, cd->points[i], 0.05f);
            insp_track_edit();
            ImGui::PopID();
        }
        if (cd->shape == JCE_COLLIDER_2D_POLYGON)
            ImGui::TextDisabled(jce_editor_i18n("inspector.c2d.polygonNote"));
        else
            ImGui::TextDisabled(jce_editor_i18n("inspector.c2d.edgeNote"));
        break;
    }
    }
}

static void draw_comp_rigidbody2d(JceRigidBody2DComponent *rb)
{
    if (!rb) return;
    static const char *body_types[] = { "Static", "Kinematic", "Dynamic" };
    int bt = (int)rb->body_type; if (bt < 0 || bt > 2) bt = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.rb2d.bodyType", "rb2d"), &bt, body_types, 3)) {
        rb->body_type = (uint8_t)bt;
        insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rb2d.mass", "rb2d"), &rb->mass, 0.1f, 0.0f, 10000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rb2d.friction", "rb2d"), &rb->friction, 0.01f, 0.0f, 10.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rb2d.restitution", "rb2d"), &rb->restitution, 0.01f, 0.0f, 1.0f);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.rb2d.fixedRotation", "rb2d"), &rb->fixed_rotation))
        insp_undo_bool(&rb->fixed_rotation);
}

static void draw_comp_particle_emitter(JceParticleEmitterComponent *pe)
{
    if (!pe) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.emitRate", "pe"), &pe->emit_rate, 0.5f, 0.0f, 10000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.lifetimeMin", "pe"), &pe->lifetime_min, 0.05f, 0.0f, 1000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.pe.lifetimeMax", "pe"), &pe->lifetime_max, 0.05f, 0.0f, 1000.0f);
    insp_track_edit();
    if (pe->lifetime_max < pe->lifetime_min) pe->lifetime_max = pe->lifetime_min;
    ImGui::TextDisabled(jce_editor_i18n("inspector.pe.useParticleSystemPanel"));
}

static void draw_comp_behavior_tree(JceBehaviorTree *bt)
{
    if (!bt) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.bt.active", "bt"), &bt->active))
        insp_undo_bool(&bt->active);
    ImGui::Text("%s: %u", jce_editor_i18n("inspector.bt.treeHandle"), (unsigned)bt->tree_handle_idx);
    ImGui::Text("%s: %u", jce_editor_i18n("inspector.bt.contextHandle"), (unsigned)bt->context_handle_idx);
    ImGui::TextDisabled(jce_editor_i18n("inspector.bt.editInBtEditor"));
}

/* ── P2-C renderer components ────────────────────────────────────── */

static void draw_comp_trail_renderer(JceTrailRendererComponent *t)
{
    if (!t) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.trail.material", "trail"), t->material_path, sizeof t->material_path);
    insp_track_edit();
    accept_asset_drop(t->material_path, sizeof t->material_path);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.trail.time", "trail"), &t->time, 0.05f, 0.0f, 600.0f, "%.2fs"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.trail.minVertexDistance", "trail"), &t->min_vertex_distance, 0.01f, 0.0f, 100.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.trail.widthStart", "trail"), &t->width_start, 0.01f, 0.0f, 100.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.trail.widthEnd", "trail"),   &t->width_end,   0.01f, 0.0f, 100.0f, "%.3f"); insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.trail.colorStart", "trail"), t->color_start); insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.trail.colorEnd", "trail"),   t->color_end);   insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trail.emitting", "trail"), &t->emitting))         insp_undo_bool(&t->emitting);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.trail.autodestruct", "trail"), &t->autodestruct)) insp_undo_bool(&t->autodestruct);
    ImGui::TextDisabled("%s: %d / %d", jce_editor_i18n("inspector.trail.capturedPoints"), t->point_count, JCE_TRAIL_MAX_POINTS);
}

static void draw_comp_line_renderer(JceLineRendererComponent *l)
{
    if (!l) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.line.material", "line"), l->material_path, sizeof l->material_path);
    insp_track_edit();
    accept_asset_drop(l->material_path, sizeof l->material_path);
    int n = l->position_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.line.positions", "line"), &n, 1.0f, 0, JCE_LINE_MAX_POINTS)) {
        if (n < 0) n = 0; if (n > JCE_LINE_MAX_POINTS) n = JCE_LINE_MAX_POINTS;
        l->position_count = n; insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.line.widthStart", "line"), &l->width_start, 0.01f, 0.0f, 100.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.line.widthEnd", "line"),   &l->width_end,   0.01f, 0.0f, 100.0f, "%.3f"); insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.line.colorStart", "line"), l->color_start); insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.line.colorEnd", "line"),   l->color_end);   insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.line.useWorldSpace", "line"), &l->use_world_space))
        insp_undo_bool(&l->use_world_space);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.line.loop", "line"), &l->loop))
        insp_undo_bool(&l->loop);
    if (ImGui::TreeNode(jce_editor_i18n_id("inspector.line.points", "line"))) {
        char lbl[32];
        for (int i = 0; i < l->position_count; ++i) {
            snprintf(lbl, sizeof lbl, "P%d##line%d", i, i);
            ImGui::DragFloat3(lbl, l->positions[i], 0.05f);
            insp_track_edit();
        }
        ImGui::TreePop();
    }
}

static void draw_comp_reflection_probe(JceReflectionProbeComponent *r)
{
    if (!r) return;
    static const char *modes[] = { "Baked", "Realtime", "Custom" };
    int m = r->mode; if (m < 0 || m > 2) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.refl.mode", "refl"), &m, modes, 3)) { r->mode = m; insp_track_edit(); }
    static const int res_choices[] = { 16, 32, 64, 128, 256, 512, 1024 };
    int ri = 3;
    for (int i = 0; i < 7; ++i) if (res_choices[i] == r->resolution) { ri = i; break; }
    if (ImGui::Combo(jce_editor_i18n_id("inspector.refl.resolution", "refl"), &ri, "16\0" "32\0" "64\0" "128\0" "256\0" "512\0" "1024\0\0")) {
        r->resolution = res_choices[ri]; insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.intensity", "refl"),      &r->intensity,      0.05f, 0.0f, 100.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.blendDistance", "refl"), &r->blend_distance, 0.05f, 0.0f, 100.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.refl.boxSize", "refl"),   r->box_size,   0.1f); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.refl.boxOffset", "refl"), r->box_offset, 0.05f); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.nearClip", "refl"), &r->near_clip, 0.01f, 0.001f, 10.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.refl.farClip", "refl"),  &r->far_clip,  1.0f, 0.1f, 100000.0f, "%.1f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.refl.boxProjection", "refl"), &r->box_projection)) insp_undo_bool(&r->box_projection);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.refl.hdr", "refl"), &r->hdr)) insp_undo_bool(&r->hdr);
    if (r->mode == JCE_REFLECTION_PROBE_CUSTOM) {
        ImGui::InputText(jce_editor_i18n_id("inspector.refl.customHdr", "refl"), r->custom_hdr_path, sizeof r->custom_hdr_path);
        insp_track_edit();
        accept_asset_drop(r->custom_hdr_path, sizeof r->custom_hdr_path);
    }
}

static void draw_comp_decal(JceDecalComponent *d)
{
    if (!d) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.decal.material", "decal"), d->material_path, sizeof d->material_path);
    insp_track_edit();
    accept_asset_drop(d->material_path, sizeof d->material_path);
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.decal.size", "decal"),  d->size,  0.05f, 0.0f, 1000.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.decal.pivot", "decal"), d->pivot, 0.05f); insp_track_edit();
    ImGui::ColorEdit4(jce_editor_i18n_id("inspector.decal.color", "decal"), d->color); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.decal.opacity", "decal"),       &d->opacity,       0.01f, 0.0f, 1.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.decal.drawDistance", "decal"), &d->draw_distance, 1.0f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.decal.fadeFactor", "decal"),   &d->fade_factor,   0.01f, 0.0f, 1.0f, "%.2f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.decal.layerMask", "decal"),    &d->layer_mask,    1.0f, -1, 0xFFFFFF); insp_track_edit();
}

static void draw_comp_light_probe_group(JceLightProbeGroupComponent *g)
{
    if (!g) return;
    int n = g->probe_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.lpg.probeCount", "lpg"), &n, 1.0f, 0, JCE_LIGHT_PROBE_MAX)) {
        if (n < 0) n = 0; if (n > JCE_LIGHT_PROBE_MAX) n = JCE_LIGHT_PROBE_MAX;
        g->probe_count = n; insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lpg.dering", "lpg"), &g->dering)) insp_undo_bool(&g->dering);
    if (ImGui::TreeNode(jce_editor_i18n_id("inspector.lpg.probes", "lpg"))) {
        char lbl[32];
        for (int i = 0; i < g->probe_count; ++i) {
            snprintf(lbl, sizeof lbl, "Probe %d##lpg%d", i, i);
            ImGui::DragFloat3(lbl, g->positions[i], 0.05f);
            insp_track_edit();
        }
        ImGui::TreePop();
    }
}

static void draw_comp_audio_listener(JceAudioListenerComponent *l)
{
    if (!l) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.al.volume", "al"), &l->volume, 0.01f, 0.0f, 1.0f, "%.2f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.al.paused", "al"), &l->paused))         insp_undo_bool(&l->paused);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.al.spatialize", "al"), &l->spatialize)) insp_undo_bool(&l->spatialize);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.al.dopplerFactor", "al"), &l->doppler_factor, 0.01f, 0.0f, 10.0f, "%.2f"); insp_track_edit();
    ImGui::TextDisabled(jce_editor_i18n("inspector.al.singletonNote"));
}

static void draw_comp_audio_reverb_zone(JceAudioReverbZoneComponent *r)
{
    if (!r) return;
    static const char *presets[] = {
        "Off", "Generic", "Padded Cell", "Room", "Bathroom", "Living Room",
        "Stone Room", "Auditorium", "Concert Hall", "Cave", "Arena", "Hangar",
        "Hallway", "Stone Corridor", "Alley", "Forest", "City", "Mountains",
        "Quarry", "Plain", "Parking Lot", "Sewer Pipe", "Underwater",
        "(reserved 23)", "(reserved 24)", "(reserved 25)", "User"
    };
    int p = r->preset; if (p < 0 || p > 26) p = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.rz.preset", "rz"), &p, presets, IM_ARRAYSIZE(presets))) {
        r->preset = p; insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.minDistance", "rz"), &r->min_distance, 0.1f, 0.0f, 100000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.maxDistance", "rz"), &r->max_distance, 0.1f, 0.0f, 100000.0f, "%.2f"); insp_track_edit();
    if (r->preset == JCE_REVERB_ZONE_PRESET_USER) {
        ImGui::Separator();
        ImGui::TextDisabled(jce_editor_i18n("inspector.rz.customParams"));
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.room", "rz"),          &r->room,              1.0f, -10000.0f, 0.0f,    "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.roomHf", "rz"),       &r->room_hf,           1.0f, -10000.0f, 0.0f,    "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.decayTime", "rz"),     &r->decay_time,        0.01f, 0.1f, 20.0f,       "%.2f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.decayHfRatio", "rz"),     &r->decay_hf_ratio,    0.01f, 0.1f, 2.0f,        "%.2f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reflections", "rz"),   &r->reflections,       1.0f, -10000.0f, 1000.0f, "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reflectionsDelay", "rz"),  &r->reflections_delay, 0.001f, 0.0f, 0.3f,       "%.3f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reverb", "rz"),        &r->reverb,            1.0f, -10000.0f, 2000.0f, "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reverbDelay", "rz"),       &r->reverb_delay,      0.001f, 0.0f, 0.1f,       "%.3f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.hfReference", "rz"),  &r->hf_reference,      10.0f, 1000.0f, 20000.0f, "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.diffusion", "rz"),     &r->diffusion,         1.0f, 0.0f, 100.0f,       "%.1f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.density", "rz"),       &r->density,           1.0f, 0.0f, 100.0f,       "%.1f"); insp_track_edit();
    }
}

static void draw_comp_audio_occlusion(JceAudioOcclusionComponent *o)
{
    if (!o) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.ao.radius", "ao"),            &o->radius,            0.1f,  0.0f, 100000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.ao.attenuation", "ao"),  &o->attenuation_db,    0.1f, -96.0f, 0.0f,     "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.ao.lowpassCutoff", "ao"), &o->lowpass_cutoff_hz, 10.0f, 20.0f, 22000.0f, "%.0f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.ao.layerMask", "ao"),        &o->layer_mask,        1.0f, -1, 0xFFFFFF); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.ao.affectsReverb", "ao"), &o->affects_reverb))
        insp_undo_bool(&o->affects_reverb);
}

static void draw_comp_spawn_manager(JceSpawnManagerComponent *m)
{
    if (!m) return;
    bool en = m->enabled != 0;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sm.enabled", "sm"), &en)) { m->enabled = en ? 1 : 0; insp_track_edit(); }
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sm.maxPeds", "sm"),          &m->max_peds,         1.0f, 0, 1024); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sm.maxVehicles", "sm"),      &m->max_vehicles,     1.0f, 0, 1024); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.minSpawnRadius", "sm"),  &m->min_spawn_radius, 0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.maxSpawnRadius", "sm"),  &m->max_spawn_radius, 0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.despawnPad", "sm"),       &m->despawn_pad,      0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sm.spawnInterval", "sm"),&m->spawn_interval,   0.05f, 0.0f, 60.0f,    "%.2f"); insp_track_edit();
    if (m->max_spawn_radius < m->min_spawn_radius) m->max_spawn_radius = m->min_spawn_radius;

    int pn = m->ped_archetype_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.pedArchetypes", "sm"), &pn, 1.0f, 0, 8)) {
        if (pn < 0) pn = 0; if (pn > 8) pn = 8;
        m->ped_archetype_count = pn; insp_track_edit();
    }
    char lbl[32];
    for (int i = 0; i < m->ped_archetype_count; ++i) {
        snprintf(lbl, sizeof lbl, "Ped[%d] id##sm%d", i, i);
        int v = (int)m->ped_archetypes[i];
        if (ImGui::DragInt(lbl, &v, 1.0f, 0, INT_MAX)) {
            m->ped_archetypes[i] = (uint32_t)(v < 0 ? 0 : v);
            insp_track_edit();
        }
    }

    int vn = m->vehicle_archetype_count;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.vehicleArchetypes", "sm"), &vn, 1.0f, 0, 8)) {
        if (vn < 0) vn = 0; if (vn > 8) vn = 8;
        m->vehicle_archetype_count = vn; insp_track_edit();
    }
    for (int i = 0; i < m->vehicle_archetype_count; ++i) {
        snprintf(lbl, sizeof lbl, "Vehicle[%d] id##smv%d", i, i);
        int v = (int)m->vehicle_archetypes[i];
        if (ImGui::DragInt(lbl, &v, 1.0f, 0, INT_MAX)) {
            m->vehicle_archetypes[i] = (uint32_t)(v < 0 ? 0 : v);
            insp_track_edit();
        }
    }

    /* RNG seed (display as two 32-bit halves to avoid ImGui int64 absence). */
    uint32_t lo = (uint32_t)(m->rng_seed & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(m->rng_seed >> 32);
    int lo_i = (int)lo, hi_i = (int)hi;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.rngSeedLo", "sm"), &lo_i, 1.0f)) {
        m->rng_seed = ((uint64_t)(uint32_t)hi_i << 32) | (uint32_t)lo_i;
        insp_track_edit();
    }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.sm.rngSeedHi", "sm"), &hi_i, 1.0f)) {
        m->rng_seed = ((uint64_t)(uint32_t)hi_i << 32) | (uint32_t)lo_i;
        insp_track_edit();
    }
    ImGui::TextDisabled(jce_editor_i18n("inspector.sm.roadNetworkNote"));
}

static void draw_comp_weapon(JceWeaponComponent *w)
{
    if (!w) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.wp.name", "wp"), w->name, sizeof w->name); insp_track_edit();
    static const char *kinds[] = { "Hitscan", "Projectile" };
    int k = w->kind; if (k < 0 || k > 1) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.wp.kind", "wp"), &k, kinds, 2)) { w->kind = k; insp_track_edit(); }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.damage", "wp"),         &w->damage,          0.1f, 0.0f, 100000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.range", "wp"),      &w->range,           0.5f, 0.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.rpm", "wp"),            &w->rpm,             1.0f, 0.0f, 10000.0f,  "%.0f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.wp.clipSize", "wp"),      &w->clip_size,       1.0f, 0, 10000); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.wp.reserveMax", "wp"),    &w->reserve_max,     1.0f, 0, 1000000); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.reload", "wp"),     &w->reload_seconds,  0.05f, 0.0f, 60.0f,    "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.spread", "wp"),   &w->spread_deg,      0.05f, 0.0f, 90.0f,    "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.recoilShot", "wp"),    &w->recoil_per_shot, 0.05f, 0.0f, 90.0f,    "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.recoilRecovery", "wp"),&w->recoil_recovery, 0.1f, 0.0f, 360.0f,    "%.2f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.wp.pellets", "wp"),        &w->pellets,         1.0f, 1, 64); insp_track_edit();
    if (w->kind == JCE_WEAPON_COMP_PROJECTILE) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.wp.projectileSpeed", "wp"), &w->projectile_speed, 1.0f, 0.0f, 10000.0f, "%.1f");
        insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.wp.fullAuto", "wp"), &w->full_auto)) insp_undo_bool(&w->full_auto);
}

static void draw_comp_save_point(JceSavePointComponent *sp)
{
    if (!sp) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.sv.saveId", "sv"),     sp->save_id,      sizeof sp->save_id);      insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.sv.displayName", "sv"),sp->display_name, sizeof sp->display_name); insp_track_edit();
    static const char *kinds[] = { "Manual", "Auto", "Checkpoint" };
    int k = sp->kind; if (k < 0 || k > 2) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.sv.kind", "sv"), &k, kinds, 3)) { sp->kind = k; insp_track_edit(); }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.sv.radius", "sv"), &sp->radius, 0.05f, 0.0f, 1000.0f, "%.2f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.sv.slot", "sv"),   &sp->slot,   1.0f, -1, 256);              insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sv.oneShot", "sv"),          &sp->one_shot))         insp_undo_bool(&sp->one_shot);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.sv.requireInteract", "sv"),  &sp->require_interact)) insp_undo_bool(&sp->require_interact);
}

/* ── P2 add-on component drawers ───────────────────────────────── */

static void draw_comp_wheel_collider(JceWheelColliderComponent *w)
{
    if (!w) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.radius", "wc"),               &w->radius,                0.01f, 0.01f, 100.0f,   "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionDistance", "wc"),  &w->suspension_distance,   0.01f, 0.0f, 10.0f,     "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionSpring", "wc"),    &w->suspension_spring,     50.0f, 0.0f, 1.0e7f,    "%.0f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionDamper", "wc"),    &w->suspension_damper,     10.0f, 0.0f, 1.0e6f,    "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionTargetPos", "wc"),&w->suspension_target_pos, 0.01f, 0.0f, 1.0f,      "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.mass", "wc"),                 &w->mass,                  0.1f, 0.001f, 100000.0f,"%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.forwardFriction", "wc"),     &w->forward_friction,      0.01f, 0.0f, 10.0f,     "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.sidewaysFriction", "wc"),    &w->sideways_friction,     0.01f, 0.0f, 10.0f,     "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.wc.center", "wc"),              w->center,                 0.01f, -100.0f, 100.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.motorTorque", "wc"),         &w->motor_torque,          1.0f, -100000.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.brakeTorque", "wc"),         &w->brake_torque,          1.0f, 0.0f, 100000.0f,  "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.steerAngle", "wc"),    &w->steer_angle_deg,       0.5f, -90.0f, 90.0f,    "%.2f"); insp_track_edit();
}

static void draw_comp_constant_force(JceConstantForceComponent *cf)
{
    if (!cf) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cf.enabled", "cf"), &cf->enabled)) insp_undo_bool(&cf->enabled);
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.force", "cf"),          cf->force,           0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.relativeForce", "cf"), cf->relative_force,  0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.torque", "cf"),         cf->torque,          0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.relativeTorque", "cf"),cf->relative_torque, 0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
}

static void draw_comp_configurable_joint(JceConfigurableJointComponent *cj)
{
    if (!cj) return;
    int connected = (int)cj->connected_body;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.cjj.connectedBody", "cjj"), &connected, 1.0f, 0, 1<<30)) {
        cj->connected_body = (uint64_t)(connected < 0 ? 0 : connected);
        insp_track_edit();
    }
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cjj.anchor", "cjj"),            cj->anchor,           0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cjj.connectedAnchor", "cjj"),  cj->connected_anchor, 0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    static const char *motion_names[] = { "Locked", "Limited", "Free" };
    ImGui::TextUnformatted(jce_editor_i18n("inspector.cjj.linearMotion"));
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_lx.x", "cjj_lx"), &cj->x_motion, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_ly.y", "cjj_ly"), &cj->y_motion, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_lz.z", "cjj_lz"), &cj->z_motion, motion_names, 3)) insp_track_edit();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.cjj.angularMotion"));
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_ax.x", "cjj_ax"), &cj->x_rotation, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_ay.y", "cjj_ay"), &cj->y_rotation, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_az.z", "cjj_az"), &cj->z_rotation, motion_names, 3)) insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.linearLimit", "cjj"),   &cj->linear_limit,        0.01f, 0.0f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.angularXLimit", "cjj"),&cj->angular_x_limit_deg, 0.5f, 0.0f, 180.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.angularYLimit", "cjj"),&cj->angular_y_limit_deg, 0.5f, 0.0f, 180.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.angularZLimit", "cjj"),&cj->angular_z_limit_deg, 0.5f, 0.0f, 180.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.breakForce", "cjj"),    &cj->break_force,  10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.breakTorque", "cjj"),   &cj->break_torque, 10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cjj.enableCollision", "cjj"), &cj->enable_collision)) insp_undo_bool(&cj->enable_collision);
}

static void draw_comp_joint2d(JceJoint2DComponent *j)
{
    if (!j) return;
    static const char *kinds[] = { "Distance", "Hinge", "Spring" };
    int k = j->kind; if (k < 0 || k > 2) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.j2d.kind", "j2d"), &k, kinds, 3)) { j->kind = k; insp_track_edit(); }
    int connected = (int)j->connected_body;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.j2d.connectedBody", "j2d"), &connected, 1.0f, 0, 1<<30)) {
        j->connected_body = (uint64_t)(connected < 0 ? 0 : connected);
        insp_track_edit();
    }
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.j2d.anchor", "j2d"),            j->anchor,           0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.j2d.connectedAnchor", "j2d"),  j->connected_anchor, 0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    if (j->kind == JCE_JOINT_2D_DISTANCE || j->kind == JCE_JOINT_2D_SPRING) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.distance", "j2d"), &j->distance, 0.01f, 0.0f, 1.0e6f, "%.3f"); insp_track_edit();
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.autoConfigureDistance", "j2d"), &j->auto_configure_distance)) insp_undo_bool(&j->auto_configure_distance);
    }
    if (j->kind == JCE_JOINT_2D_SPRING) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.frequency", "j2d"),     &j->frequency,     0.05f, 0.0f, 10000.0f, "%.3f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.dampingRatio", "j2d"), &j->damping_ratio, 0.01f, 0.0f, 1.0f,     "%.3f"); insp_track_edit();
    }
    if (j->kind == JCE_JOINT_2D_HINGE) {
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.useMotor", "j2d"), &j->use_motor)) insp_undo_bool(&j->use_motor);
        if (j->use_motor) {
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.motorSpeed", "j2d"), &j->motor_speed_deg_s, 1.0f, -3600.0f, 3600.0f, "%.1f"); insp_track_edit();
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.motorMaxTorque", "j2d"),    &j->motor_max_torque,  10.0f, 0.0f, 1.0e7f,     "%.1f"); insp_track_edit();
        }
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.useLimits", "j2d"), &j->use_limits)) insp_undo_bool(&j->use_limits);
        if (j->use_limits) {
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.lowerAngle", "j2d"), &j->lower_angle_deg, 0.5f, -360.0f, 360.0f, "%.2f"); insp_track_edit();
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.upperAngle", "j2d"), &j->upper_angle_deg, 0.5f, -360.0f, 360.0f, "%.2f"); insp_track_edit();
        }
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.breakForce", "j2d"),  &j->break_force,  10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.breakTorque", "j2d"), &j->break_torque, 10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.enableCollision", "j2d"), &j->enable_collision)) insp_undo_bool(&j->enable_collision);
}

static void draw_comp_billboard_renderer(JceBillboardRendererComponent *b)
{
    if (!b) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.br.texturePath", "br"), b->texture_path, sizeof b->texture_path); insp_track_edit();
    static const char *modes[] = { "Full", "Y-Axis Only" };
    int m = b->mode; if (m < 0 || m > 1) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.br.mode", "br"), &m, modes, 2)) { b->mode = m; insp_track_edit(); }
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.br.size", "br"), b->size, 0.01f, 0.0f, 1.0e4f, "%.3f"); insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.br.color", "br"), b->color)) insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.br.visible", "br"), &b->visible)) insp_undo_bool(&b->visible);
}

static void draw_comp_canvas(JceCanvasComponent *cv)
{
    if (!cv) return;
    static const char *modes[] = { "Screen Space - Overlay", "Screen Space - Camera", "World Space" };
    int m = cv->render_mode; if (m < 0 || m > 2) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cv.renderMode", "cv"), &m, modes, 3)) { cv->render_mode = m; insp_track_edit(); }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.cv.sortOrder", "cv"), &cv->sort_order, 1.0f, -32768, 32767)) insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.cv.referenceResolution", "cv"), cv->reference_resolution, 1.0f, 1.0f, 16384.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cv.scaleFactor", "cv"), &cv->scale_factor, 0.01f, 0.0001f, 1000.0f, "%.4f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cv.pixelPerfect", "cv"), &cv->pixel_perfect)) insp_undo_bool(&cv->pixel_perfect);
}

static void draw_comp_canvas_group(JceCanvasGroupComponent *cg)
{
    if (!cg) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cg.alpha", "cg"), &cg->alpha, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cg.interactable", "cg"),         &cg->interactable))          insp_undo_bool(&cg->interactable);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cg.blocksRaycasts", "cg"),      &cg->blocks_raycasts))       insp_undo_bool(&cg->blocks_raycasts);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cg.ignoreParentGroups", "cg"), &cg->ignore_parent_groups))  insp_undo_bool(&cg->ignore_parent_groups);
}

static void draw_comp_layout_group(JceLayoutGroupComponent *lg)
{
    if (!lg) return;
    static const char *kinds[] = { "Horizontal", "Vertical", "Grid" };
    int k = lg->layout_kind; if (k < 0 || k > 2) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.lg.layout", "lg"), &k, kinds, 3)) { lg->layout_kind = k; insp_track_edit(); }
    ImGui::DragFloat4(jce_editor_i18n_id("inspector.lg.padding", "lg"), lg->padding, 1.0f, 0.0f, 4096.0f, "%.0f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.spacing", "lg"), lg->spacing, 0.5f, 0.0f, 4096.0f, "%.1f"); insp_track_edit();
    if (lg->layout_kind == JCE_LAYOUT_GRID) {
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.lg.cellSize", "lg"), lg->cell_size, 1.0f, 1.0f, 4096.0f, "%.0f"); insp_track_edit();
    }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.lg.childAlignment", "lg"), &lg->child_alignment, 1.0f, 0, 8)) insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.controlChildWidth", "lg"),   &lg->control_child_size_w)) insp_undo_bool(&lg->control_child_size_w);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.controlChildHeight", "lg"),  &lg->control_child_size_h)) insp_undo_bool(&lg->control_child_size_h);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lg.reverseArrangement", "lg"),   &lg->reverse_arrangement))  insp_undo_bool(&lg->reverse_arrangement);
}

static void draw_comp_ui_image(JceUIImageComponent *im)
{
    if (!im) return;
    ImGui::InputText(jce_editor_i18n_id("inspector.uim.spritePath", "uim"), im->sprite_path, sizeof im->sprite_path); insp_track_edit();
    static const char *types[] = { "Simple", "Sliced", "Tiled", "Filled" };
    int t = im->image_type; if (t < 0 || t > 3) t = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uim.imageType", "uim"), &t, types, 4)) { im->image_type = t; insp_track_edit(); }
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uim.color", "uim"), im->color)) insp_track_edit();
    if (im->image_type == JCE_UI_IMAGE_FILLED) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.uim.fillAmount", "uim"), &im->fill_amount, 0.01f, 0.0f, 1.0f, "%.3f"); insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uim.preserveAspect", "uim"), &im->preserve_aspect)) insp_undo_bool(&im->preserve_aspect);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uim.raycastTarget", "uim"),  &im->raycast_target))  insp_undo_bool(&im->raycast_target);
}

static void draw_comp_ui_text(JceUITextComponent *tx)
{
    if (!tx) return;
    ImGui::InputTextMultiline(jce_editor_i18n_id("inspector.uit.text", "uit"), tx->text, sizeof tx->text, ImVec2(0, ImGui::GetTextLineHeight() * 4)); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uit.fontPath", "uit"), tx->font_path, sizeof tx->font_path); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uit.fontSize", "uit"), &tx->font_size, 0.5f, 1.0f, 512.0f, "%.1f"); insp_track_edit();
    static const char *aligns[] = { "Left", "Center", "Right" };
    int a = tx->alignment; if (a < 0 || a > 2) a = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.uit.alignment", "uit"), &a, aligns, 3)) { tx->alignment = a; insp_track_edit(); }
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uit.color", "uit"), tx->color)) insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uit.lineSpacing", "uit"), &tx->line_spacing, 0.05f, 0.0f, 10.0f, "%.3f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.richText", "uit"), &tx->rich_text)) insp_undo_bool(&tx->rich_text);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uit.bestFit", "uit"),  &tx->best_fit))  insp_undo_bool(&tx->best_fit);
    if (tx->best_fit) {
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.uit.minSize", "uit"), &tx->min_size, 1.0f, 1, 512)) insp_track_edit();
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.uit.maxSize", "uit"), &tx->max_size, 1.0f, 1, 512)) insp_track_edit();
    }
}

static void draw_comp_ui_button(JceUIButtonComponent *bt)
{
    if (!bt) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.uib.interactable", "uib"), &bt->interactable)) insp_undo_bool(&bt->interactable);
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.normalColor", "uib"),      bt->normal_color))      insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.highlightedColor", "uib"), bt->highlighted_color)) insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.pressedColor", "uib"),     bt->pressed_color))     insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.uib.disabledColor", "uib"),    bt->disabled_color))    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.uib.fadeDuration", "uib"), &bt->fade_duration, 0.01f, 0.0f, 5.0f, "%.3f"); insp_track_edit();
    ImGui::InputText(jce_editor_i18n_id("inspector.uib.onClickHandler", "uib"), bt->on_click_handler, sizeof bt->on_click_handler); insp_track_edit();
}

static void draw_comp_ui_toggle(JceUIToggleComponent *t)
{
    if (!t) return;
    if (ImGui::Checkbox("Interactable##uit", &t->interactable)) insp_undo_bool(&t->interactable);
    if (ImGui::Checkbox("Is On##uit",        &t->is_on))        insp_undo_bool(&t->is_on);
    if (ImGui::InputInt ("Group ID##uit",     &t->group_id))    insp_track_edit();
    ImGui::DragFloat   ("Fade Duration##uit", &t->fade_duration, 0.01f, 0.0f, 5.0f); insp_track_edit();
    ImGui::InputText   ("On Value Changed##uit", t->on_value_changed, sizeof t->on_value_changed); insp_track_edit();
}

static void draw_comp_ui_slider(JceUISliderComponent *sl)
{
    if (!sl) return;
    if (ImGui::Checkbox ("Interactable##uisl", &sl->interactable)) insp_undo_bool(&sl->interactable);
    ImGui::DragFloat    ("Min Value##uisl",    &sl->min_value, 0.1f); insp_track_edit();
    ImGui::DragFloat    ("Max Value##uisl",    &sl->max_value, 0.1f); insp_track_edit();
    if (sl->min_value > sl->max_value) sl->max_value = sl->min_value;
    ImGui::SliderFloat  ("Value##uisl",        &sl->value, sl->min_value, sl->max_value); insp_track_edit();
    if (ImGui::Checkbox ("Whole Numbers##uisl",&sl->whole_numbers)) insp_undo_bool(&sl->whole_numbers);
    const char *dir_items[] = { "Left→Right", "Right→Left", "Bottom→Top", "Top→Bottom" };
    if (ImGui::Combo("Direction##uisl",        &sl->direction, dir_items, 4)) insp_track_edit();
    ImGui::InputText    ("On Value Changed##uisl", sl->on_value_changed, sizeof sl->on_value_changed); insp_track_edit();
}

static void draw_comp_ui_dropdown(JceUIDropdownComponent *d)
{
    if (!d) return;
    if (ImGui::Checkbox("Interactable##uid", &d->interactable)) insp_undo_bool(&d->interactable);
    ImGui::Text("Selected Index: %d", d->value);
    int oc = (int)d->option_count;
    if (ImGui::InputInt("Option Count##uid", &oc)) {
        if (oc < 0) oc = 0;
        if (oc > JCE_UI_DROPDOWN_MAX_OPTIONS) oc = JCE_UI_DROPDOWN_MAX_OPTIONS;
        d->option_count = (int)oc;
        insp_track_edit();
    }
    for (int i = 0; i < d->option_count; ++i) {
        char label[32];
        snprintf(label, sizeof(label), "Option %d##uid_o", i);
        ImGui::InputText(label, d->options[i], sizeof d->options[i]); insp_track_edit();
    }
    ImGui::InputText("On Value Changed##uid", d->on_value_changed, sizeof d->on_value_changed); insp_track_edit();
}

static void draw_comp_ui_input_field(JceUIInputFieldComponent *f)
{
    if (!f) return;
    if (ImGui::Checkbox("Interactable##uif", &f->interactable)) insp_undo_bool(&f->interactable);
    ImGui::InputInt    ("Character Limit##uif", &f->character_limit); insp_track_edit();
    const char *ct_items[] = { "Standard", "Integer", "Decimal", "Email", "Password" };
    if (ImGui::Combo   ("Content Type##uif", &f->content_type, ct_items, 5)) insp_track_edit();
    if (ImGui::Checkbox("Read Only##uif",   &f->read_only))  insp_undo_bool(&f->read_only);
    if (ImGui::Checkbox("Multi Line##uif",  &f->multi_line)) insp_undo_bool(&f->multi_line);
    ImGui::InputText   ("Text##uif",         f->text,        sizeof f->text); insp_track_edit();
    ImGui::InputText   ("Placeholder##uif",  f->placeholder, sizeof f->placeholder); insp_track_edit();
    ImGui::InputText   ("On Value Changed##uif", f->on_value_changed, sizeof f->on_value_changed); insp_track_edit();
    ImGui::InputText   ("On End Edit##uif",      f->on_end_edit,      sizeof f->on_end_edit); insp_track_edit();
}

static void draw_comp_ui_raw_image(JceUIRawImageComponent *r)
{
    if (!r) return;
    ImGui::InputText    ("Texture Path##uiri", r->texture_path, sizeof r->texture_path); insp_track_edit();
    ImGui::DragFloat2   ("UV Min (u0,v0)##uiri", r->uv_rect,     0.01f); insp_track_edit();
    ImGui::DragFloat2   ("UV Max (u1,v1)##uiri", r->uv_rect + 2, 0.01f); insp_track_edit();
    if (ImGui::ColorEdit4("Color##uiri",      r->color))           insp_track_edit();
    if (ImGui::Checkbox ("Preserve Aspect##uiri", &r->preserve_aspect)) insp_undo_bool(&r->preserve_aspect);
}

static void draw_comp_ui_outline_effect(JceUIOutlineEffect *o)
{
    if (!o) return;
    if (ImGui::ColorEdit4("Color##uioe",          o->effect_color))     insp_track_edit();
    ImGui::DragFloat2   ("Distance##uioe",        o->effect_distance, 0.5f); insp_track_edit();
    if (ImGui::Checkbox ("Use Graphic Alpha##uioe", &o->use_graphic_alpha)) insp_undo_bool(&o->use_graphic_alpha);
}

static void draw_comp_ui_shadow_effect(JceUIShadowEffect *o)
{
    if (!o) return;
    if (ImGui::ColorEdit4("Color##uise",          o->effect_color))     insp_track_edit();
    ImGui::DragFloat2   ("Distance##uise",        o->effect_distance, 0.5f); insp_track_edit();
    if (ImGui::Checkbox ("Use Graphic Alpha##uise", &o->use_graphic_alpha)) insp_undo_bool(&o->use_graphic_alpha);
}

static void draw_comp_point_light_2d(JcePointLight2DComponent *p)
{
    if (!p) return;
    if (ImGui::ColorEdit4("Color##pl2d",       p->color))            insp_track_edit();
    ImGui::DragFloat    ("Intensity##pl2d",    &p->intensity,    0.05f); insp_track_edit();
    ImGui::DragFloat    ("Outer Radius##pl2d", &p->outer_radius, 0.1f, 0.0f, 100.0f); insp_track_edit();
    ImGui::DragFloat    ("Inner Radius##pl2d", &p->inner_radius, 0.1f, 0.0f, p->outer_radius); insp_track_edit();
    int mask = (int)p->target_layer_mask;
    if (ImGui::InputInt ("Layer Mask##pl2d", &mask, 0)) {
        p->target_layer_mask = (uint32_t)mask; insp_track_edit();
    }
    const char *blend_items[] = { "Additive", "Multiply" };
    if (ImGui::Combo    ("Blend##pl2d",        &p->blend, blend_items, 2)) insp_track_edit();
    if (ImGui::Checkbox ("Volumetric##pl2d",   &p->volumetric)) insp_undo_bool(&p->volumetric);
}

static void draw_comp_spot_light_2d(JceSpotLight2DComponent *p)
{
    if (!p) return;
    if (ImGui::ColorEdit4("Color##sl2d",       p->color))            insp_track_edit();
    ImGui::DragFloat    ("Intensity##sl2d",    &p->intensity,    0.05f); insp_track_edit();
    ImGui::DragFloat    ("Outer Radius##sl2d", &p->outer_radius, 0.1f, 0.0f, 100.0f); insp_track_edit();
    ImGui::DragFloat    ("Inner Radius##sl2d", &p->inner_radius, 0.1f, 0.0f, p->outer_radius); insp_track_edit();
    ImGui::DragFloat    ("Inner Angle##sl2d",  &p->inner_angle_deg, 1.0f, 0.0f, 180.0f); insp_track_edit();
    ImGui::DragFloat    ("Outer Angle##sl2d",  &p->outer_angle_deg, 1.0f, p->inner_angle_deg, 180.0f); insp_track_edit();
    int mask = (int)p->target_layer_mask;
    if (ImGui::InputInt ("Layer Mask##sl2d", &mask, 0)) {
        p->target_layer_mask = (uint32_t)mask; insp_track_edit();
    }
    const char *blend_items[] = { "Additive", "Multiply" };
    if (ImGui::Combo    ("Blend##sl2d",        &p->blend, blend_items, 2)) insp_track_edit();
    if (ImGui::Checkbox ("Volumetric##sl2d",   &p->volumetric)) insp_undo_bool(&p->volumetric);
}

static void draw_comp_global_light_2d(JceGlobalLight2DComponent *p)
{
    if (!p) return;
    if (ImGui::ColorEdit4("Color##gl2d",       p->color)) insp_track_edit();
    ImGui::DragFloat    ("Intensity##gl2d",    &p->intensity, 0.05f, 0.0f, 4.0f); insp_track_edit();
    int mask = (int)p->target_layer_mask;
    if (ImGui::InputInt ("Layer Mask##gl2d", &mask, 0)) {
        p->target_layer_mask = (uint32_t)mask; insp_track_edit();
    }
    const char *blend_items[] = { "Additive", "Multiply" };
    if (ImGui::Combo    ("Blend##gl2d",        &p->blend, blend_items, 2)) insp_track_edit();
}

/* ── Component header / settings popup helper ─────────────────────── */

/* Returns true if the component's body should be drawn this frame.
 * Updates sidecar.expanded_flags fold state.  Handles the "..." popup
 * with a Remove menu (disabled when not removable, e.g. Transform). */
static bool comp_section_begin(uint32_t entity_id,
                               EditorEntitySidecar &sidecar,
                               uint64_t flag,
                               const char *display_name,
                               bool removable)
{
    ImGui::PushID((int)(flag ^ (flag >> 32)));

    bool was_open = (sidecar.expanded_flags & flag) != 0;
    int tn_flags = ImGuiTreeNodeFlags_AllowOverlap |
                   (was_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);

    /* Inspector collapsing-header tint:
       - Dark themes: keep the slate slate-blue accent (#323744) so it
         reads as a distinct band over the dark window bg.
       - Light themes: defer to ImGuiCol_Header so the bar tracks the
         active palette (avoids a near-black strip on white). */
    ImGui::PushStyleColor(ImGuiCol_Header, jce_theme::inspector_header_color());
    bool open = ImGui::CollapsingHeader(display_name, tn_flags);
    if (open) sidecar.expanded_flags |= flag;
    else      sidecar.expanded_flags &= ~flag;

    /* Drag-reorder: pressing & dragging a header begins a drag; while
     * active, hovering another header records it as the drop target. On
     * mouse release the loop applies the move. */
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
        if (!s_drag.active) {
            s_drag.active   = true;
            s_drag.entity_id = entity_id;
            s_drag.src_flag  = flag;
        }
    }
    if (s_drag.active && s_drag.entity_id == entity_id && ImGui::IsItemHovered()) {
        s_drag.hover_flag = flag;
        /* Visual cue: thin line above the hovered header. */
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(mn.x, mn.y), ImVec2(mx.x, mn.y),
            ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
    }

    float header_w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(header_w - 20);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
        JceScene *_cs = jce_state_get_scene();
        JceEntity _ce = jce_state_to_ecs_entity(entity_id);
        size_t _csz = 0;
        void *_cptr = comp_get_ptr_and_size(_cs, _ce, flag, &_csz);

        if (ImGui::MenuItem(jce_editor_i18n("inspector.copyComponent"),
                            NULL, false, _cptr != NULL && _csz > 0 && _csz <= sizeof(s_comp_clipboard.data))) {
            s_comp_clipboard.flag = flag;
            s_comp_clipboard.data_size = _csz;
            memcpy(s_comp_clipboard.data, _cptr, _csz);
        }
        bool can_paste = (s_comp_clipboard.flag == flag && s_comp_clipboard.data_size > 0
                          && _cptr != NULL && _csz == s_comp_clipboard.data_size);
        if (!can_paste) ImGui::BeginDisabled();
        if (ImGui::MenuItem(jce_editor_i18n("inspector.pasteComponentValues"))) {
            jce_state_begin_batch_edit();
            memcpy(_cptr, s_comp_clipboard.data, s_comp_clipboard.data_size);
            jce_state_end_batch_edit();
        }
        if (!can_paste) ImGui::EndDisabled();

        ImGui::Separator();

        /* Move Up / Move Down — defer to end-of-frame loop. */
        if (ImGui::MenuItem(jce_editor_i18n("inspector.moveUp"))) {
            s_pending_move.entity_id = entity_id;
            s_pending_move.src_flag  = flag;
            s_pending_move.dir       = -1;
            s_pending_move.pending   = true;
        }
        if (ImGui::MenuItem(jce_editor_i18n("inspector.moveDown"))) {
            s_pending_move.entity_id = entity_id;
            s_pending_move.src_flag  = flag;
            s_pending_move.dir       = +1;
            s_pending_move.pending   = true;
        }

        ImGui::Separator();

        /* Preset submenu — save the focused entity's component values to
         * a named preset, or apply a previously saved preset. The actual
         * OpenPopup must run outside BeginMenu (different ID-stack) so the
         * matching BeginPopup below can find it. */
        bool open_preset_save = false;
        if (ImGui::BeginMenu(jce_editor_i18n("inspector.preset"))) {
            if (ImGui::MenuItem(jce_editor_i18n("inspector.preset.saveAs"))) {
                open_preset_save = true;
            }
            ImGui::Separator();
            std::vector<std::string> names = jce_preset_list(flag);
            if (names.empty()) {
                ImGui::TextDisabled("%s", jce_editor_i18n("inspector.preset.empty"));
            } else {
                std::string pending_delete;
                for (const auto &n : names) {
                    if (ImGui::BeginMenu(n.c_str())) {
                        if (ImGui::MenuItem(jce_editor_i18n("inspector.preset.apply"))) {
                            jce_preset_apply(flag, n.c_str(), _cs, _ce);
                        }
                        ImGui::Separator();
                        ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
                        if (ImGui::MenuItem(jce_editor_i18n("inspector.preset.delete"))) {
                            pending_delete = n;
                        }
                        ImGui::PopStyleColor();
                        ImGui::EndMenu();
                    }
                }
                if (!pending_delete.empty()) {
                    jce_preset_delete(flag, pending_delete.c_str());
                }
            }
            ImGui::EndMenu();
        }
        if (open_preset_save) {
            s_preset_save_buf[0] = '\0';
            ImGui::OpenPopup("##preset_save_popup");
        }

        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("transform.reset"))) {
            if (_cptr && _csz > 0) {
                jce_state_begin_batch_edit();
                if (flag == JCE_COMP_FLAG_TRANSFORM) {
                    JceTransform *t = (JceTransform *)_cptr;
                    t->position = { 0.0f, 0.0f, 0.0f };
                    t->rotation = jce_q_identity();
                    t->scale    = { 1.0f, 1.0f, 1.0f };
                } else {
                    memset(_cptr, 0, _csz);
                }
                jce_state_end_batch_edit();
            }
        }

        ImGui::Separator();

        if (!removable) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent"), NULL, false, false);
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            if (ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent"))) {
                s_pending_remove.entity_id = entity_id;
                s_pending_remove.flag      = flag;
                s_pending_remove.pending   = true;
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();

    /* Preset save modal — shared per-section so opening one closes others. */
    if (ImGui::BeginPopup("##preset_save_popup")) {
        ImGui::Text(jce_editor_i18n("inspector.preset.savePromptFmt"),
                    jce_comp_flag_display_name(flag));
        ImGui::SetNextItemWidth(220);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool commit = ImGui::InputText("##preset_name", s_preset_save_buf,
                                       sizeof(s_preset_save_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(jce_editor_i18n("dialog.save")) || commit) {
            if (s_preset_save_buf[0]) {
                JceScene *_ps = jce_state_get_scene();
                JceEntity _pe = jce_state_to_ecs_entity(entity_id);
                jce_preset_save(flag, s_preset_save_buf, _ps, _pe);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("dialog.cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    return open;
}

static void comp_section_end(void)
{
    ImGui::Spacing();
    ImGui::PopID();
}

/* ══════════════════════════════════════════════════════════════════════
 *  COMPONENT DISPLAY ORDER + DISPATCH
 *
 *  Inspector iterates components in user-controlled order stored in
 *  EditorEntitySidecar.component_order.  We use a synthetic "Light Group"
 *  flag (high bit) so the unified Light section (dir/point/spot) gets
 *  one slot that survives switching light type.
 * ══════════════════════════════════════════════════════════════════════ */

static constexpr uint64_t LIGHT_GROUP_BIT = (UINT64_C(1) << 63);
static constexpr uint64_t LIGHT_MASK      = JCE_COMP_FLAG_DIR_LIGHT |
                                            JCE_COMP_FLAG_POINT_LIGHT |
                                            JCE_COMP_FLAG_SPOT_LIGHT;

/* Default ordering follows the historic hard-coded layout. */
static const uint64_t kDefaultComponentOrder[] = {
    JCE_COMP_FLAG_TRANSFORM,
    LIGHT_GROUP_BIT,
    JCE_COMP_FLAG_CAMERA,
    JCE_COMP_FLAG_MESH_RENDERER,
    JCE_COMP_FLAG_SPRITE_RENDERER,
    JCE_COMP_FLAG_ANIMATOR,
    JCE_COMP_FLAG_SKELETAL_ANIMATOR,
    JCE_COMP_FLAG_RIGIDBODY,
    JCE_COMP_FLAG_BOX_COLLIDER,
    JCE_COMP_FLAG_SPHERE_COLLIDER,
    JCE_COMP_FLAG_CHARACTER_CONTROLLER,
    JCE_COMP_FLAG_AUDIO_SOURCE,
    JCE_COMP_FLAG_SCRIPT,
    JCE_COMP_FLAG_SKYBOX,
    JCE_COMP_FLAG_SPRITE_ANIMATOR,
    JCE_COMP_FLAG_CONSTRAINT,
    JCE_COMP_FLAG_TERRAIN,
    JCE_COMP_FLAG_RIGIDBODY_2D,
    JCE_COMP_FLAG_PARTICLE_EMITTER,
    JCE_COMP_FLAG_BEHAVIOR_TREE,
    JCE_COMP_FLAG_LOD_GROUP,
    JCE_COMP_FLAG_VIRTUAL_CAMERA,
    JCE_COMP_FLAG_TRIGGER_VOLUME,
    JCE_COMP_FLAG_CAPSULE_COLLIDER,
    JCE_COMP_FLAG_MESH_COLLIDER,
    JCE_COMP_FLAG_COLLIDER_2D,
    JCE_COMP_FLAG_TRAIL_RENDERER,
    JCE_COMP_FLAG_LINE_RENDERER,
    JCE_COMP_FLAG_REFLECTION_PROBE,
    JCE_COMP_FLAG_DECAL,
    JCE_COMP_FLAG_LIGHT_PROBE_GROUP,
    JCE_COMP_FLAG_AUDIO_LISTENER,
    JCE_COMP_FLAG_AUDIO_REVERB_ZONE,
    JCE_COMP_FLAG_AUDIO_OCCLUSION,
    JCE_COMP_FLAG_SPAWN_MANAGER,
    JCE_COMP_FLAG_WEAPON,
    JCE_COMP_FLAG_SAVE_POINT,
    JCE_COMP_FLAG_WHEEL_COLLIDER,
    JCE_COMP_FLAG_CONSTANT_FORCE,
    JCE_COMP_FLAG_CONFIGURABLE_JOINT,
    JCE_COMP_FLAG_JOINT_2D,
    JCE_COMP_FLAG_BILLBOARD_RENDERER,
    JCE_COMP_FLAG_CANVAS,
    JCE_COMP_FLAG_CANVAS_GROUP,
    JCE_COMP_FLAG_LAYOUT_GROUP,
    JCE_COMP_FLAG_UI_IMAGE,
    JCE_COMP_FLAG_UI_TEXT,
    JCE_COMP_FLAG_UI_BUTTON,
};

/* Ensures sidecar.component_order contains exactly the slots we want to
 * draw, given the entity's currently-set component flags:
 *   - Removes entries no longer present (component was removed).
 *   - Appends new entries in default-order positions (component added).
 *   - Light flags collapse into the synthetic LIGHT_GROUP_BIT slot. */
static void sync_component_order(EditorEntitySidecar &sidecar, uint64_t flags)
{
    auto wanted = [&](uint64_t entry) -> bool {
        if (entry == LIGHT_GROUP_BIT) return (flags & LIGHT_MASK) != 0;
        /* Skip raw light flags — they live under LIGHT_GROUP_BIT. */
        if (entry & LIGHT_MASK) return false;
        return (flags & entry) != 0;
    };

    /* Drop stale entries while preserving order of survivors. */
    auto &v = sidecar.component_order;
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](uint64_t f) { return !wanted(f); }),
            v.end());

    /* Add any missing entries by walking the default order. */
    for (uint64_t def : kDefaultComponentOrder) {
        if (!wanted(def)) continue;
        if (std::find(v.begin(), v.end(), def) == v.end())
            v.push_back(def);
    }
}

static void draw_one_component_section(uint32_t focused,
                                       EditorEntitySidecar &sidecar,
                                       JceScene *scene,
                                       JceEntity ecs_e,
                                       uint64_t flag);

/* ══════════════════════════════════════════════════════════════════════
 *  MULTI-OBJECT EDITING
 *  component flag, edits made through the focused entity's section are
 *  byte-mirrored onto the others. We restrict the broadcast to a
 *  whitelist of pure value-type components — string buffers and asset
 *  handles inside other components must be edited per-entity.
 * ══════════════════════════════════════════════════════════════════════ */

static bool multi_edit_supported(uint64_t flag)
{
    switch (flag) {
        case JCE_COMP_FLAG_TRANSFORM:
        case JCE_COMP_FLAG_CAMERA:
        case JCE_COMP_FLAG_MESH_RENDERER:
        case JCE_COMP_FLAG_SPRITE_RENDERER:
        case JCE_COMP_FLAG_RIGIDBODY:
        case JCE_COMP_FLAG_BOX_COLLIDER:
        case JCE_COMP_FLAG_SPHERE_COLLIDER:
        case JCE_COMP_FLAG_CAPSULE_COLLIDER:
        case JCE_COMP_FLAG_MESH_COLLIDER:
        case JCE_COMP_FLAG_AUDIO_SOURCE:
        case JCE_COMP_FLAG_CONSTRAINT:
        case JCE_COMP_FLAG_SKELETAL_ANIMATOR:
            return true;
        default:
            return false;
    }
}

static void *multi_get_comp_ptr(JceScene *scene, JceEntity e,
                                uint64_t flag, size_t *out_size)
{
#define M(F, GETTER, T)                                                   \
    case F: {                                                             \
        T *p = GETTER(scene, e);                                          \
        if (out_size) *out_size = sizeof(T);                              \
        return (void *)p;                                                 \
    }
    switch (flag) {
        M(JCE_COMP_FLAG_TRANSFORM,         jce_scene_get_transform,         JceTransform)
        M(JCE_COMP_FLAG_CAMERA,            jce_scene_get_camera,            JceCameraComponent)
        M(JCE_COMP_FLAG_MESH_RENDERER,     jce_scene_get_mesh_renderer,     JceMeshRenderer)
        M(JCE_COMP_FLAG_SPRITE_RENDERER,   jce_scene_get_sprite_renderer,   JceSpriteRendererComponent)
        M(JCE_COMP_FLAG_RIGIDBODY,         jce_scene_get_rigidbody,         JceRigidBodyComponent)
        M(JCE_COMP_FLAG_BOX_COLLIDER,      jce_scene_get_box_collider,      JceBoxColliderComponent)
        M(JCE_COMP_FLAG_SPHERE_COLLIDER,   jce_scene_get_sphere_collider,   JceSphereColliderComponent)
        M(JCE_COMP_FLAG_CAPSULE_COLLIDER,  jce_scene_get_capsule_collider,  JceCapsuleColliderComponent)
        M(JCE_COMP_FLAG_MESH_COLLIDER,     jce_scene_get_mesh_collider,     JceMeshColliderComponent)
        M(JCE_COMP_FLAG_AUDIO_SOURCE,      jce_scene_get_audio_source,      JceAudioSourceComponent)
        M(JCE_COMP_FLAG_CONSTRAINT,        jce_scene_get_constraint,        JceConstraintComponent)
        M(JCE_COMP_FLAG_SKELETAL_ANIMATOR, jce_scene_get_skeletal_animator, JceSkeletalAnimatorComponent)
        default:
            if (out_size) *out_size = 0;
            return nullptr;
    }
#undef M
}

/* Wrap a single-component draw with a before/after byte diff and broadcast
 * the diff to every other selected entity that holds the same flag. */
static void draw_section_with_multi_broadcast(uint32_t focused,
                                              EditorEntitySidecar &sidecar,
                                              JceScene *scene,
                                              JceEntity ecs_e,
                                              uint64_t entry)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    bool multi = (sel_count > 1) && multi_edit_supported(entry);

    void *focused_ptr = nullptr;
    size_t comp_size = 0;
    std::vector<uint8_t> before;
    if (multi) {
        focused_ptr = multi_get_comp_ptr(scene, ecs_e, entry, &comp_size);
        if (focused_ptr && comp_size > 0)
            before.assign((uint8_t *)focused_ptr,
                          (uint8_t *)focused_ptr + comp_size);
    }

    draw_one_component_section(focused, sidecar, scene, ecs_e, entry);

    if (!multi || !focused_ptr || comp_size == 0) return;
    if (memcmp(focused_ptr, before.data(), comp_size) == 0) return;

    /* Focused changed during this draw — broadcast the new bytes to peers. */
    for (int i = 0; i < sel_count; ++i) {
        uint32_t other = sel[i];
        if (other == focused) continue;
        JceEntity oe = jce_state_to_ecs_entity(other);
        if (!oe) continue;
        uint64_t oflags = jce_scene_get_component_flags(scene, oe);
        if (!(oflags & entry)) continue;
        size_t osize = 0;
        void *optr = multi_get_comp_ptr(scene, oe, entry, &osize);
        if (optr && osize == comp_size)
            memcpy(optr, focused_ptr, comp_size);
    }
}

/* Apply a pending Move Up / Move Down menu action or drag-reorder drop.
 * Called once per inspector frame after the iteration loop. */
static void apply_pending_reorder(uint32_t focused_entity,
                                  EditorEntitySidecar &sidecar)
{
    /* Drag drop: on mouse release, move src before/after hover. */
    if (s_drag.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (s_drag.entity_id == focused_entity &&
            s_drag.src_flag != s_drag.hover_flag &&
            s_drag.hover_flag != 0) {
            auto &v = sidecar.component_order;
            auto it_src = std::find(v.begin(), v.end(), s_drag.src_flag);
            auto it_dst = std::find(v.begin(), v.end(), s_drag.hover_flag);
            if (it_src != v.end() && it_dst != v.end()) {
                jce_state_begin_batch_edit();
                uint64_t f = *it_src;
                size_t dst_idx = (size_t)(it_dst - v.begin());
                v.erase(it_src);
                if (dst_idx > (size_t)(it_src - v.begin())) dst_idx--;
                v.insert(v.begin() + dst_idx, f);
                jce_state_end_batch_edit();
            }
        }
        s_drag = { 0, 0, 0, false };
    }

    if (!s_pending_move.pending) return;
    if (s_pending_move.entity_id != focused_entity) {
        s_pending_move.pending = false;
        return;
    }

    auto &v = sidecar.component_order;
    auto it = std::find(v.begin(), v.end(), s_pending_move.src_flag);
    if (it != v.end()) {
        size_t idx = (size_t)(it - v.begin());
        if (s_pending_move.dir < 0 && idx > 0) {
            jce_state_begin_batch_edit();
            std::swap(v[idx], v[idx - 1]);
            jce_state_end_batch_edit();
        } else if (s_pending_move.dir > 0 && idx + 1 < v.size()) {
            jce_state_begin_batch_edit();
            std::swap(v[idx], v[idx + 1]);
            jce_state_end_batch_edit();
        }
    }
    s_pending_move = { 0, 0, 0, 0, false };
}

/* Single dispatch from a flag value to the matching draw_comp_X call.
 * Mirrors the historic per-flag if-block sequence verbatim so behaviour
 * is byte-identical except for ordering. Light is handled inline by the
 * caller (LIGHT_GROUP_BIT path). */
static void draw_one_component_section(uint32_t focused,
                                       EditorEntitySidecar &sidecar,
                                       JceScene *scene,
                                       JceEntity ecs_e,
                                       uint64_t flag)
{
    const char *nm = jce_comp_flag_display_name(flag);
    bool removable = (flag != JCE_COMP_FLAG_TRANSFORM);

#define JCE_DRAW(F, EXPR)                                                  \
    case F:                                                                \
        if (comp_section_begin(focused, sidecar, F, nm, removable)) EXPR;  \
        comp_section_end();                                                \
        break

    switch (flag) {
        JCE_DRAW(JCE_COMP_FLAG_TRANSFORM,
                 draw_comp_transform(focused, jce_scene_get_transform(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CAMERA,
                 draw_comp_camera(jce_scene_get_camera(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_MESH_RENDERER,
                 draw_comp_mesh_renderer(jce_scene_get_mesh_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPRITE_RENDERER,
                 draw_comp_sprite_renderer(jce_scene_get_sprite_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_ANIMATOR,
                 draw_comp_animator(jce_scene_get_animator(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SKELETAL_ANIMATOR,
                 draw_comp_skeletal_animator(jce_scene_get_skeletal_animator(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_RIGIDBODY,
                 draw_comp_rigidbody(jce_scene_get_rigidbody(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_BOX_COLLIDER,
                 draw_comp_box_collider(jce_scene_get_box_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPHERE_COLLIDER,
                 draw_comp_sphere_collider(jce_scene_get_sphere_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CHARACTER_CONTROLLER,
                 draw_comp_character_controller(jce_scene_get_character_controller(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_SOURCE,
                 draw_comp_audio_source(jce_scene_get_audio_source(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SCRIPT,
                 draw_comp_script(jce_scene_get_script(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SKYBOX,
                 draw_comp_skybox(jce_scene_get_skybox(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPRITE_ANIMATOR,
                 draw_comp_sprite_animator(jce_scene_get_sprite_animator(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CONSTRAINT,
                 draw_comp_constraint(jce_scene_get_constraint(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TERRAIN,
                 draw_comp_terrain(jce_scene_get_terrain(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_RIGIDBODY_2D,
                 draw_comp_rigidbody2d(jce_scene_get_rigidbody2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_PARTICLE_EMITTER,
                 draw_comp_particle_emitter(jce_scene_get_particle_emitter(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_BEHAVIOR_TREE,
                 draw_comp_behavior_tree(jce_scene_get_behavior_tree(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LOD_GROUP,
                 draw_comp_lod_group(jce_scene_get_lod_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_VIRTUAL_CAMERA,
                 draw_comp_virtual_camera(jce_scene_get_virtual_camera(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TRIGGER_VOLUME,
                 draw_comp_trigger_volume(jce_scene_get_trigger_volume(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CAPSULE_COLLIDER,
                 draw_comp_capsule_collider(jce_scene_get_capsule_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_MESH_COLLIDER,
                 draw_comp_mesh_collider(jce_scene_get_mesh_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_COLLIDER_2D,
                 draw_comp_collider2d(jce_scene_get_collider2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_TRAIL_RENDERER,
                 draw_comp_trail_renderer(jce_scene_get_trail_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LINE_RENDERER,
                 draw_comp_line_renderer(jce_scene_get_line_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_REFLECTION_PROBE,
                 draw_comp_reflection_probe(jce_scene_get_reflection_probe(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_DECAL,
                 draw_comp_decal(jce_scene_get_decal(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LIGHT_PROBE_GROUP,
                 draw_comp_light_probe_group(jce_scene_get_light_probe_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_LISTENER,
                 draw_comp_audio_listener(jce_scene_get_audio_listener(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_REVERB_ZONE,
                 draw_comp_audio_reverb_zone(jce_scene_get_audio_reverb_zone(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_AUDIO_OCCLUSION,
                 draw_comp_audio_occlusion(jce_scene_get_audio_occlusion(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPAWN_MANAGER,
                 draw_comp_spawn_manager(jce_scene_get_spawn_manager(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_WEAPON,
                 draw_comp_weapon(jce_scene_get_weapon(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SAVE_POINT,
                 draw_comp_save_point(jce_scene_get_save_point(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_WHEEL_COLLIDER,
                 draw_comp_wheel_collider(jce_scene_get_wheel_collider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CONSTANT_FORCE,
                 draw_comp_constant_force(jce_scene_get_constant_force(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CONFIGURABLE_JOINT,
                 draw_comp_configurable_joint(jce_scene_get_configurable_joint(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_JOINT_2D,
                 draw_comp_joint2d(jce_scene_get_joint2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_BILLBOARD_RENDERER,
                 draw_comp_billboard_renderer(jce_scene_get_billboard_renderer(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CANVAS,
                 draw_comp_canvas(jce_scene_get_canvas(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_CANVAS_GROUP,
                 draw_comp_canvas_group(jce_scene_get_canvas_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_LAYOUT_GROUP,
                 draw_comp_layout_group(jce_scene_get_layout_group(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_IMAGE,
                 draw_comp_ui_image(jce_scene_get_ui_image(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_TEXT,
                 draw_comp_ui_text(jce_scene_get_ui_text(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_BUTTON,
                 draw_comp_ui_button(jce_scene_get_ui_button(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_TOGGLE,
                 draw_comp_ui_toggle(jce_scene_get_ui_toggle(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_SLIDER,
                 draw_comp_ui_slider(jce_scene_get_ui_slider(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_DROPDOWN,
                 draw_comp_ui_dropdown(jce_scene_get_ui_dropdown(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_INPUT_FIELD,
                 draw_comp_ui_input_field(jce_scene_get_ui_input_field(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_RAW_IMAGE,
                 draw_comp_ui_raw_image(jce_scene_get_ui_raw_image(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_OUTLINE_EFFECT,
                 draw_comp_ui_outline_effect(jce_scene_get_ui_outline_effect(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_UI_SHADOW_EFFECT,
                 draw_comp_ui_shadow_effect(jce_scene_get_ui_shadow_effect(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_POINT_LIGHT_2D,
                 draw_comp_point_light_2d(jce_scene_get_point_light_2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_SPOT_LIGHT_2D,
                 draw_comp_spot_light_2d(jce_scene_get_spot_light_2d(scene, ecs_e)));
        JCE_DRAW(JCE_COMP_FLAG_GLOBAL_LIGHT_2D,
                 draw_comp_global_light_2d(jce_scene_get_global_light_2d(scene, ecs_e)));
        default: break;
    }
#undef JCE_DRAW
}

/* ── Material file sync ───────────────────────────────────────────── */

void jce_editor_inspector_reload_material(const char *material_path)
{
    if (!material_path || material_path[0] == '\0') return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int total = jce_state_get_entity_count();
    for (int i = 0; i < total; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (!id) continue;
        JceEntity e = jce_state_to_ecs_entity(id);
        if (!jce_scene_has_mesh_renderer(scene, e)) continue;
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr && strcmp(mr->material_path, material_path) == 0)
            load_material_into_renderer(mr);
    }
}

/* ── Add Component menu options ───────────────────────────────────── */

struct AddCompOption {
    uint64_t flag;
    bool     is_light;       /* True for the 3 light flags (light section). */
    bool     is_collider;    /* True for box/sphere collider (mutually exclusive). */
};

static const AddCompOption s_add_options[] = {
    { JCE_COMP_FLAG_MESH_RENDERER,        false, false },
    { JCE_COMP_FLAG_CAMERA,               false, false },
    { JCE_COMP_FLAG_DIR_LIGHT,            true,  false },
    { JCE_COMP_FLAG_POINT_LIGHT,          true,  false },
    { JCE_COMP_FLAG_SPOT_LIGHT,           true,  false },
    { JCE_COMP_FLAG_SKYBOX,               false, false },
    { JCE_COMP_FLAG_SPRITE_RENDERER,      false, false },
    { JCE_COMP_FLAG_SPRITE_ANIMATOR,      false, false },
    { JCE_COMP_FLAG_ANIMATOR,             false, false },
    { JCE_COMP_FLAG_SKELETAL_ANIMATOR,    false, false },
    { JCE_COMP_FLAG_RIGIDBODY,            false, false },
    { JCE_COMP_FLAG_RIGIDBODY_2D,         false, false },
    { JCE_COMP_FLAG_BOX_COLLIDER,         false, true  },
    { JCE_COMP_FLAG_SPHERE_COLLIDER,      false, true  },
    { JCE_COMP_FLAG_CHARACTER_CONTROLLER, false, false },
    { JCE_COMP_FLAG_AUDIO_SOURCE,         false, false },
    { JCE_COMP_FLAG_SCRIPT,               false, false },
    { JCE_COMP_FLAG_CONSTRAINT,           false, false },
    { JCE_COMP_FLAG_TERRAIN,              false, false },
    { JCE_COMP_FLAG_PARTICLE_EMITTER,     false, false },
    { JCE_COMP_FLAG_BEHAVIOR_TREE,        false, false },
    { JCE_COMP_FLAG_LOD_GROUP,            false, false },
    { JCE_COMP_FLAG_VIRTUAL_CAMERA,       false, false },
    { JCE_COMP_FLAG_TRIGGER_VOLUME,       false, false },
    { JCE_COMP_FLAG_CAPSULE_COLLIDER,     false, false },
    { JCE_COMP_FLAG_MESH_COLLIDER,        false, false },
    { JCE_COMP_FLAG_COLLIDER_2D,          false, false },
    { JCE_COMP_FLAG_TRAIL_RENDERER,       false, false },
    { JCE_COMP_FLAG_LINE_RENDERER,        false, false },
    { JCE_COMP_FLAG_REFLECTION_PROBE,     false, false },
    { JCE_COMP_FLAG_DECAL,                false, false },
    { JCE_COMP_FLAG_LIGHT_PROBE_GROUP,    false, false },
    { JCE_COMP_FLAG_AUDIO_LISTENER,       false, false },
    { JCE_COMP_FLAG_AUDIO_REVERB_ZONE,    false, false },
    { JCE_COMP_FLAG_AUDIO_OCCLUSION,      false, false },
    { JCE_COMP_FLAG_SPAWN_MANAGER,        false, false },
    { JCE_COMP_FLAG_WEAPON,               false, false },
    { JCE_COMP_FLAG_SAVE_POINT,           false, false },
    { JCE_COMP_FLAG_WHEEL_COLLIDER,       false, false },
    { JCE_COMP_FLAG_CONSTANT_FORCE,       false, false },
    { JCE_COMP_FLAG_CONFIGURABLE_JOINT,   false, false },
    { JCE_COMP_FLAG_JOINT_2D,             false, false },
    { JCE_COMP_FLAG_BILLBOARD_RENDERER,   false, false },
    { JCE_COMP_FLAG_CANVAS,               false, false },
    { JCE_COMP_FLAG_CANVAS_GROUP,         false, false },
    { JCE_COMP_FLAG_LAYOUT_GROUP,         false, false },
    { JCE_COMP_FLAG_UI_IMAGE,             false, false },
    { JCE_COMP_FLAG_UI_TEXT,              false, false },
    { JCE_COMP_FLAG_UI_BUTTON,            false, false },
};

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_inspector_content(void)
{
    ensure_init();
    char lbl[128];

    JceScene *scene = jce_state_get_scene();

    /* Play-mode warning: edits in PLAY mode will be reverted on STOP. */
    {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_PLAYING || ps == JCE_PLAY_PAUSED) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.10f, 1.0f));
            ImGui::TextWrapped("%s", jce_editor_i18n("inspector.playModeWarning"));
            ImGui::PopStyleColor();
            ImGui::Separator();
        }
    }

    /* ── Multi-entity selection header ─────────────────────────────── */
    int sel_count = 0;
    const uint32_t *sel_ids = jce_state_get_selection(&sel_count);

    if (sel_count > 1) {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                           "%d %s", sel_count, jce_editor_i18n("inspector.entitiesSelected"));
        ImGui::Separator();

        bool all_enabled = true, all_disabled = true;
        for (int i = 0; i < sel_count; i++) {
            if (!jce_state_entity_exists(sel_ids[i])) continue;
            bool en = jce_state_entity_enabled(sel_ids[i]);
            if (en)  all_disabled = false;
            else     all_enabled  = false;
        }

        bool mixed = (!all_enabled && !all_disabled);
        bool chk = all_enabled;
        if (mixed) {
            ImGui::Text("%s", jce_editor_i18n("inspector.enabledMixed"));
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("inspector.enableAll")))
                for (int i = 0; i < sel_count; i++)
                    jce_state_set_entity_enabled(sel_ids[i], true);
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("inspector.disableAll")))
                for (int i = 0; i < sel_count; i++)
                    jce_state_set_entity_enabled(sel_ids[i], false);
        } else {
            snprintf(lbl, sizeof(lbl), "%s###multi_enabled", jce_editor_i18n("inspector.enabled"));
            if (ImGui::Checkbox(lbl, &chk)) {
                for (int i = 0; i < sel_count; i++)
                    jce_state_set_entity_enabled(sel_ids[i], chk);
            }
        }

        bool same_color = jce_state_entity_exists(sel_ids[0]);
        JceTagColor first_color = same_color ? jce_state_entity_tag_color(sel_ids[0])
                                             : JCE_TAG_NONE;
        for (int i = 1; same_color && i < sel_count; i++) {
            if (!jce_state_entity_exists(sel_ids[i])) continue;
            if (jce_state_entity_tag_color(sel_ids[i]) != first_color) {
                same_color = false;
                break;
            }
        }

        if (same_color) {
            int tag_color = (int)first_color;
            const char *tag_items[] = {
                jce_editor_i18n("hierarchy.noTag"),
                jce_editor_i18n("hierarchy.tag.red"),
                jce_editor_i18n("hierarchy.tag.orange"),
                jce_editor_i18n("hierarchy.tag.yellow"),
                jce_editor_i18n("hierarchy.tag.green"),
                jce_editor_i18n("hierarchy.tag.blue"),
                jce_editor_i18n("hierarchy.tag.purple"),
                jce_editor_i18n("hierarchy.tag.gray")
            };
            ImGui::PushItemWidth(-1);
            snprintf(lbl, sizeof(lbl), "%s###multi_tag_color", jce_editor_i18n("inspector.tagColor"));
            if (ImGui::Combo(lbl, &tag_color, tag_items, 8)) {
                for (int i = 0; i < sel_count; i++)
                    jce_state_set_entity_tag_color(sel_ids[i], (JceTagColor)tag_color);
            }
            ImGui::PopItemWidth();
        } else {
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.mixedTagColors"));
        }

        ImGui::Separator();

        /* ── Bulk Transform editor (multi-select) ──────────────────── */
        if (scene) {
            if (ImGui::CollapsingHeader(jce_editor_i18n("inspector.bulk.section"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushID("##jce_bulk_xform_section");
                static float pos_delta[3] = {0,0,0};
                static float rot_set[3]   = {0,0,0};
                static float scl_set[3]   = {1,1,1};
                static bool  scl_uniform  = true;

                ImGui::TextDisabled("%s", jce_editor_i18n("inspector.bulk.posOffset"));
                ImGui::DragFloat3("##bulk_pos_delta", pos_delta, 0.1f);
                ImGui::SameLine();
                if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.apply", "bulk_pos"))) {
                    for (int i = 0; i < sel_count; i++) {
                        JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                        JceTransform *t = jce_scene_get_transform(scene, e);
                        if (t) {
                            t->position.x += pos_delta[0];
                            t->position.y += pos_delta[1];
                            t->position.z += pos_delta[2];
                        }
                    }
                    pos_delta[0] = pos_delta[1] = pos_delta[2] = 0.0f;
                }

                ImGui::TextDisabled("%s", jce_editor_i18n("inspector.bulk.rotAbsolute"));
                ImGui::DragFloat3("##bulk_rot_set", rot_set, 1.0f);
                ImGui::SameLine();
                if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.apply", "bulk_rot"))) {
                    for (int i = 0; i < sel_count; i++) {
                        JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                        JceTransform *t = jce_scene_get_transform(scene, e);
                        if (t) {
                            t->rotation = editor_q_from_euler_deg(rot_set);
                            jce_editor_set_cached_euler_deg(sel_ids[i], t->rotation, rot_set);
                        }
                    }
                }

                ImGui::TextDisabled("%s", jce_editor_i18n("inspector.bulk.scaleAbsolute"));
                ImGui::Checkbox(jce_editor_i18n_id("inspector.bulk.uniform", "bulk_scl_uni"), &scl_uniform);
                if (scl_uniform) {
                    ImGui::DragFloat("##bulk_scl_uniform_v", &scl_set[0], 0.01f, 0.001f, 1000.0f);
                    scl_set[1] = scl_set[2] = scl_set[0];
                } else {
                    ImGui::DragFloat3("##bulk_scl_set", scl_set, 0.01f, 0.001f, 1000.0f);
                }
                ImGui::SameLine();
                if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.apply", "bulk_scl"))) {
                    for (int i = 0; i < sel_count; i++) {
                        JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                        JceTransform *t = jce_scene_get_transform(scene, e);
                        if (t) {
                            t->scale.x = scl_set[0];
                            t->scale.y = scl_set[1];
                            t->scale.z = scl_set[2];
                        }
                    }
                }

                ImGui::Separator();
                if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.resetPos", "bulk_reset_pos"))) {
                    for (int i = 0; i < sel_count; i++) {
                        JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                        JceTransform *t = jce_scene_get_transform(scene, e);
                        if (t) { t->position.x = t->position.y = t->position.z = 0.0f; }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.resetScale", "bulk_reset_scl"))) {
                    for (int i = 0; i < sel_count; i++) {
                        JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                        JceTransform *t = jce_scene_get_transform(scene, e);
                        if (t) { t->scale.x = t->scale.y = t->scale.z = 1.0f; }
                    }
                }
                ImGui::PopID();
            }

            ImGui::Separator();
        }

        {
            uint32_t focused_id = jce_state_get_focused();
            uint32_t to_focus = 0;
            uint32_t to_remove = 0;
            int max_show = sel_count < 20 ? sel_count : 20;
            for (int i = 0; i < max_show; i++) {
                ImGui::PushID(i);
                uint32_t id = sel_ids[i];
                const char *nm = jce_state_entity_name(id);
                if (!nm) nm = "(unnamed)";
                bool is_focused = (id == focused_id);

                /* Reserve room for the two SmallButtons on the right so
                 * the Selectable does not (a) push them past the visible
                 * region and (b) intercept their clicks across the row. */
                const char *focus_lbl  = jce_editor_i18n_id("inspector.bulk.entity.focus", "focus");
                const char *remove_lbl = jce_editor_i18n_id("inspector.bulk.entity.remove", "remove");
                ImGuiStyle &style = ImGui::GetStyle();
                float btn_focus_w  = ImGui::CalcTextSize(focus_lbl).x  + style.FramePadding.x * 2.0f;
                float btn_remove_w = ImGui::CalcTextSize(remove_lbl).x + style.FramePadding.x * 2.0f;
                float reserved     = btn_focus_w + btn_remove_w + style.ItemSpacing.x * 2.0f;
                float avail_w      = ImGui::GetContentRegionAvail().x;
                float sel_w        = avail_w - reserved;
                if (sel_w < 32.0f) sel_w = 32.0f;

                /* Selectable: click to make this the focused entity within the multi-selection. */
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::Selectable(nm, is_focused,
                                      ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(sel_w, 0.0f))) {
                    to_focus = id;
                }
                ImGui::SameLine();
                /* Frame camera on this entity. */
                if (ImGui::SmallButton(focus_lbl)) {
                    JceEntity e = jce_state_to_ecs_entity(id);
                    JceTransform *t = jce_scene_get_transform(scene, e);
                    if (t) jce_editor_scene_camera_set_target(t->position.x, t->position.y, t->position.z);
                    to_focus = id;
                }
                ImGui::SameLine();
                /* Remove from current selection. */
                if (ImGui::SmallButton(remove_lbl)) {
                    to_remove = id;
                }
                ImGui::PopID();
            }
            if (sel_count > 20)
                ImGui::Text("... %d %s", sel_count - 20, jce_editor_i18n("inspector.andMore"));

            if (to_remove) jce_state_deselect_entity(to_remove);
            else if (to_focus) {
                /* Re-focus within multi-selection: just retarget the
                 * focused id; do NOT remove+re-add, which would shove the
                 * clicked entity to the tail of the selection array and
                 * make this very list visually "swap" the clicked row
                 * with the bottom row on every click. */
                jce_state_set_focused(to_focus);
            }
        }

        ImGui::Separator();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        float btn_w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(jce_editor_i18n("inspector.deleteSelected"), ImVec2(btn_w, 0)))
            jce_editor_inspector_request_delete_confirm_many(sel_ids, sel_count);
        ImGui::PopStyleColor(2);

        return;
    }

    uint32_t focused = jce_state_get_focused();
    if (!focused || !jce_state_entity_exists(focused) || !scene) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.noSelection"));
        return;
    }

    const char *ent_name    = jce_state_entity_name(focused);
    bool        ent_enabled = jce_state_entity_enabled(focused);
    const char *ent_tag     = jce_state_entity_tag(focused);
    JceTagColor ent_tcolor  = jce_state_entity_tag_color(focused);

    /* Prefab inheritance badge: shows the parent prefab path if this
     * entity was created as a Variant.  Mirrors Unity's
     * "Variant of <prefab>" hint at the top of the Inspector. */
    {
        JceEditorMeta *m_top = jce_scene_get_editor_meta(
            scene, jce_state_to_ecs_entity(focused));
        if (m_top) {
            const char *base_path = NULL;
            if (m_top->variant_parent_path[0]) {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                                   "Variant of: %s", m_top->variant_parent_path);
                base_path = m_top->variant_parent_path;
            } else if (m_top->prefab_instance && m_top->prefab_path[0]) {
                ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.0f),
                                   "Prefab: %s", m_top->prefab_path);
                base_path = m_top->prefab_path;
            }

            if (base_path) {
                /* Apply / Revert buttons.  Operate on the editor-wide
                 * override session keyed by entity name (the same
                 * convention prefab_overrides uses for entity_path).
                 * "Apply All" rewrites the base prefab file with every
                 * recorded override; "Revert" drops overrides for this
                 * specific entity. */
                const char *ent_path = m_top->name[0] ? m_top->name : "";
                uint32_t over_count = jce_editor_prefab_overrides_count(ent_path);
                if (over_count > 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
                                       "(%u override%s)",
                                       over_count, over_count == 1 ? "" : "s");
                }
                if (ImGui::SmallButton("Apply All to Base")) {
                    if (jce_editor_prefab_overrides_apply(base_path)) {
                        /* Successful apply clears the session set. */
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert Overrides")) {
                    jce_editor_prefab_overrides_revert(ent_path);
                }
            }
        }
    }

    if (s_insp.needs_sync) {
        snprintf(s_insp.name_buf, sizeof(s_insp.name_buf), "%s", ent_name ? ent_name : "");
        snprintf(s_insp.tag_buf,  sizeof(s_insp.tag_buf),  "%s", ent_tag  ? ent_tag  : "");
        s_insp.needs_sync = false;
    }

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 50);
    if (ImGui::InputText("##name", s_insp.name_buf, sizeof(s_insp.name_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        jce_state_rename_entity(focused, s_insp.name_buf);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("inspector.delete")))
        jce_editor_inspector_request_delete_confirm(focused);
    ImGui::PopStyleColor(2);

    bool enabled = ent_enabled;
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###enabled", jce_editor_i18n("inspector.enabled"));
        if (ImGui::Checkbox(_lbl, &enabled))
            jce_state_set_entity_enabled(focused, enabled);
    }
    ImGui::SameLine();

    int tag_color = (int)ent_tcolor;
    float combo_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushItemWidth(combo_w);
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###TagColor", jce_editor_i18n("inspector.tagColor"));
        static const char *kTagColors[] = {
            "tagColor.none", "tagColor.red", "tagColor.orange", "tagColor.yellow",
            "tagColor.green", "tagColor.blue", "tagColor.purple", "tagColor.gray"
        };
        if (ImGui::Combo(_lbl, &tag_color, jce_editor_i18n_combo(kTagColors, 8)))
            jce_state_set_entity_tag_color(focused, (JceTagColor)tag_color);
    }
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(-1);
    {
        const JceProjectSettings *ps = jce_project_settings_current();
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###tag", jce_editor_i18n("inspector.tag"));

        /* Tag field — InputText so users can type a new tag inline (Enter
           commits and registers it in project settings). A small "▾"
           button next to the field opens a dropdown of existing tags. */
        const float arrow_w = ImGui::GetFrameHeight();
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float label_w = ImGui::CalcTextSize(jce_editor_i18n("inspector.tag")).x;
        const float field_w = ImGui::GetContentRegionAvail().x - arrow_w - spacing - label_w - spacing;
        ImGui::PushItemWidth(field_w > 80.0f ? field_w : 80.0f);
        bool tag_commit = ImGui::InputTextWithHint(
            "##tagInput",
            jce_editor_i18n_or("inspector.tagAdd.untagged", "Untagged"),
            s_insp.tag_buf, sizeof(s_insp.tag_buf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        if (tag_commit) {
            /* Register the typed tag if it's new. */
            if (s_insp.tag_buf[0] != '\0' && ps) {
                bool exists = false;
                for (int i = 0; i < ps->tags_layers.tag_count && i < JCE_PS_MAX_TAGS; i++) {
                    if (strcmp(ps->tags_layers.tags[i], s_insp.tag_buf) == 0) {
                        exists = true; break;
                    }
                }
                if (!exists && ps->tags_layers.tag_count < JCE_PS_MAX_TAGS) {
                    JceProjectSettings *pm = (JceProjectSettings *)ps;
                    snprintf(pm->tags_layers.tags[pm->tags_layers.tag_count],
                             JCE_PS_NAME_LEN, "%s", s_insp.tag_buf);
                    pm->tags_layers.tag_count++;
                    jce_project_settings_save(pm);
                }
            }
            jce_state_set_entity_tag(focused, s_insp.tag_buf);
        }
        ImGui::SameLine(0.0f, spacing);
        if (ImGui::ArrowButton("##tagPick", ImGuiDir_Down))
            ImGui::OpenPopup("##tagDropdown");
        ImGui::SameLine(0.0f, spacing);
        ImGui::TextUnformatted(jce_editor_i18n("inspector.tag"));

        if (ImGui::BeginPopup("##tagDropdown")) {
            if (ImGui::Selectable(jce_editor_i18n("inspector.tagAdd.untagged"),
                                   s_insp.tag_buf[0] == '\0')) {
                s_insp.tag_buf[0] = '\0';
                jce_state_set_entity_tag(focused, s_insp.tag_buf);
            }
            if (ps) {
                for (int i = 0; i < ps->tags_layers.tag_count && i < JCE_PS_MAX_TAGS; i++) {
                    const char *t = ps->tags_layers.tags[i];
                    if (!t || !*t) continue;
                    bool sel = (strcmp(s_insp.tag_buf, t) == 0);
                    if (ImGui::Selectable(t, sel)) {
                        snprintf(s_insp.tag_buf, sizeof(s_insp.tag_buf), "%s", t);
                        jce_state_set_entity_tag(focused, s_insp.tag_buf);
                    }
                }
            }
            ImGui::EndPopup();
        }

        /* Layer picker — combo from project layers (32 slots, 0..7 builtin). */
        JceEditorMeta *meta_for_layer = jce_scene_get_editor_meta(scene,
                                            jce_state_to_ecs_entity(focused));
        int cur_layer = meta_for_layer ? meta_for_layer->layer : 0;
        if (cur_layer < 0 || cur_layer > 31) cur_layer = 0;
        char layer_label[256];
        snprintf(layer_label, sizeof(layer_label), "%s###layer",
                 jce_editor_i18n_or("inspector.layer", "Layer"));
        const char *cur_layer_name = "Default";
        if (ps && ps->tags_layers.layers[cur_layer][0] != '\0')
            cur_layer_name = ps->tags_layers.layers[cur_layer];
        if (ImGui::BeginCombo(layer_label, cur_layer_name)) {
            /* Show only NAMED layers (Unity convention). Slot 0 falls
               back to "Default" even when its name field is empty;
               unnamed user slots are hidden — manage them in
               Project Settings → Tags & Layers. */
            for (int i = 0; i < 32; i++) {
                const char *nm = NULL;
                if (ps && ps->tags_layers.layers[i][0] != '\0')
                    nm = ps->tags_layers.layers[i];
                else if (i == 0)
                    nm = "Default";
                if (!nm) continue;
                if (ImGui::Selectable(nm, i == cur_layer))
                    jce_state_set_entity_layer(focused, i);
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    if (!ent_enabled) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.entityDisabled"));
        ImGui::Spacing();
    }

    ImGui::BeginDisabled(!ent_enabled);

    /* ── Multi-selection banner ──────────────────────────────────── */
    {
        int sel_count = 0;
        jce_state_get_selection(&sel_count);
        if (sel_count > 1) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::GetColorU32(ImGuiCol_HeaderHovered));
            ImGui::Text(jce_editor_i18n("inspector.multiEditTitle"), sel_count);
            ImGui::PopStyleColor();
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.multiEditHint"));
            ImGui::Separator();
        }
    }

    /* ── Components ───────────────────────────────────────────────── */
    JceEntity ecs_e = jce_state_to_ecs_entity(focused);
    uint64_t flags = jce_scene_get_component_flags(scene, ecs_e);
    EditorEntitySidecar &sidecar = g_entity_sidecar[focused];

    sync_component_order(sidecar, flags);

    /* Iterate components in user-defined display order. The dispatcher
     * delegates to the same comp_section_begin / draw_comp_X / end
     * sequence the previous code used per-flag. */
    bool light_drawn = false;
    for (uint64_t entry : sidecar.component_order) {
        if (entry == LIGHT_GROUP_BIT) {
            if (light_drawn) continue;
            if (!(flags & LIGHT_MASK)) continue;
            uint64_t lf = (flags & JCE_COMP_FLAG_DIR_LIGHT)   ? JCE_COMP_FLAG_DIR_LIGHT
                       : (flags & JCE_COMP_FLAG_POINT_LIGHT) ? JCE_COMP_FLAG_POINT_LIGHT
                                                              : JCE_COMP_FLAG_SPOT_LIGHT;
            if (comp_section_begin(focused, sidecar, lf, "Light", true))
                draw_comp_light(scene, ecs_e, flags);
            comp_section_end();
            light_drawn = true;
            continue;
        }
        if (!(flags & entry)) continue;
        draw_section_with_multi_broadcast(focused, sidecar, scene, ecs_e, entry);
    }

    apply_pending_reorder(focused, sidecar);


    /* ── Add Component button ─────────────────────────────────────── */
    ImGui::Spacing();
    float btn_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(jce_editor_i18n("inspector.addComponent"), ImVec2(btn_w, 0))) {
        ImGui::OpenPopup("AddComponentPopup");
    }

    /* Force a sensible default size on the popup's first appearing frame.
       Without this the popup auto-sizes to its (initially zero) content
       width, so the BeginChild list inside collapses to a few pixels and
       the items become invisible — only the *second* open recovers
       because ImGui then reuses the stored window size. */
    ImGui::SetNextWindowSize(ImVec2(280.0f, 260.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopup("AddComponentPopup")) {
        static char s_addcomp_filter[64] = {0};
        static bool s_addcomp_focus = false;
        if (ImGui::IsWindowAppearing()) {
            s_addcomp_filter[0] = '\0';
            s_addcomp_focus = true;
        }
        if (s_addcomp_focus) {
            ImGui::SetKeyboardFocusHere();
            s_addcomp_focus = false;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##addcomp_search",
            jce_editor_i18n("inspector.searchComponents"),
            s_addcomp_filter, sizeof(s_addcomp_filter));

        ImGui::BeginChild("##addcomp_list", ImVec2(0, 200), false);
        const int n_opts = (int)(sizeof(s_add_options) / sizeof(s_add_options[0]));
        int first_match = -1;
        for (int i = 0; i < n_opts; i++) {
            const AddCompOption &opt = s_add_options[i];
            if (flags & opt.flag) continue;
            if (opt.is_light && (flags & LIGHT_MASK)) continue;
            const char *cname = jce_comp_flag_display_name(opt.flag);
            if (!cname) continue;
            if (s_addcomp_filter[0]) {
                /* Match against the EN display name AND every loaded
                   locale's translation of the component's i18n key, so
                   users can search in any language present in the
                   editor's locale tables — not just the active one. */
                auto substr_ci = [](const char *hay, const char *needle) -> bool {
                    if (!hay || !needle || !*needle) return false;
                    for (const char *h = hay; *h; ++h) {
                        const char *a = h, *b = needle;
                        while (*a && *b && ((*a | 32) == (*b | 32))) { ++a; ++b; }
                        if (!*b) return true;
                    }
                    return false;
                };
                bool match = substr_ci(cname, s_addcomp_filter);
                if (!match) {
                    const char *i18n_key = jce_comp_flag_i18n_key(opt.flag);
                    if (i18n_key) {
                        int n_loc = jce_editor_i18n_locale_count();
                        for (int li = 0; li < n_loc && !match; ++li) {
                            const char *loc_name = jce_editor_i18n_lookup_locale(
                                (JceLocale)li, i18n_key);
                            if (substr_ci(loc_name, s_addcomp_filter)) match = true;
                        }
                    }
                }
                if (!match) continue;
            }
            if (first_match < 0) first_match = i;
            if (ImGui::MenuItem(cname)) {
                jce_state_add_component(focused, opt.flag);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();

        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && first_match >= 0) {
            jce_state_add_component(focused, s_add_options[first_match].flag);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::EndDisabled();

    /* Flush deferred component removal here, after all draw_comp_*
     * functions have returned (so no stale flecs pointer is in use). */
    if (s_pending_remove.pending) {
        uint32_t eid = s_pending_remove.entity_id;
        uint64_t fl  = s_pending_remove.flag;
        s_pending_remove.pending   = false;
        s_pending_remove.entity_id = 0;
        s_pending_remove.flag      = 0;
        jce_state_remove_component(eid, fl);
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_inspector(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###inspector", jce_editor_i18n("Inspector"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_inspector_content();
    ImGui::End();
}

/* ── Top-level delete confirmation dialog ─────────────────────────── */

void jce_editor_inspector_delete_dialog(void)
{
    if (!s_insp.delete_requested) return;
    if (s_insp.delete_entity_count <= 0) {
        s_insp.delete_requested = false;
        return;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();

    const char *popup_id = "###ConfirmDeleteEntityDlg";
    if (s_insp.delete_requested && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    bool keep_open = s_insp.delete_requested;

    char title[256];
    snprintf(title, sizeof(title), "%s%s", jce_editor_i18n("dialog.confirmDelete"), popup_id);
    if (!ImGui::BeginPopupModal(title,
                                        &keep_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_AlwaysAutoResize)) {
        s_insp.delete_requested = keep_open;
        return;
    }

    if (s_insp.delete_entity_count == 1)
        ImGui::TextUnformatted(jce_editor_i18n("inspector.confirmDelete"));
    else
        ImGui::Text("%s: %d",
                    jce_editor_i18n("inspector.confirmDeleteMultiple"),
                    s_insp.delete_entity_count);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w = 140.0f;
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("inspector.yes"), ImVec2(btn_w, 0))) {
        uint32_t ids[JCE_MAX_SELECTED];
        int n = s_insp.delete_entity_count;
        if (n > JCE_MAX_SELECTED) n = JCE_MAX_SELECTED;
        for (int i = 0; i < n; i++)
            ids[i] = s_insp.delete_entity_ids[i];

        if (n > 1) jce_state_begin_batch_edit();
        for (int i = 0; i < n; i++) {
            /* Skip ids that were already cascade-deleted by a prior
             * iteration (when a parent in the selection took its
             * descendants with it via flecs ChildOf cascade). */
            if (!jce_state_entity_exists(ids[i])) continue;
            jce_state_delete_entity(ids[i]);
        }
        if (n > 1) jce_state_end_batch_edit();

        keep_open = false;
        ImGui::PopStyleColor(3);
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        s_insp.delete_requested = keep_open;
        if (!keep_open) s_insp.delete_entity_count = 0;
        return;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inspector.no"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::CloseCurrentPopup();
        keep_open = false;
    }

    ImGui::EndPopup();
    s_insp.delete_requested = keep_open;
    if (!keep_open) s_insp.delete_entity_count = 0;
}
