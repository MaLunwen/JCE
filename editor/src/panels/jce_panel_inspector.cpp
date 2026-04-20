/*
 * jce_panel_inspector.cpp  Inspector panel (entity properties).
 *
 * Reads from / writes to the ECS scene directly via jce_scene_*.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_state_internal.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "scene/jce_model_loader_assimp.h"
#include "scene/jce_editor_scene_render.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cmath>

/* Per-entity euler cache shared with the scene-view gizmo. Implemented in
 * jce_scene_view_helpers.cpp; declared inline here to avoid pulling in the
 * full scene-view internal header. */
extern "C++" {
    bool jce_editor_get_cached_euler_deg(uint32_t entity_id, jce_quat current_q,
                                          float out_deg[3]);
    void jce_editor_set_cached_euler_deg(uint32_t entity_id, jce_quat q,
                                          const float deg[3]);
}

extern "C" {
#include <jce/graphics/jce_pbr_material.h>
#include <jce/graphics/jce_model.h>
#include <jce/animation/jce_animation.h>
#include <jce/scene/jce_scene.h>
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
        jce_q_to_euler_deg(t->rotation, rot);

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
        t->rotation = jce_q_from_euler_deg(rot);
        jce_editor_set_cached_euler_deg(entity_id, t->rotation, rot);
    }

    ImGui::Text("%s", jce_editor_i18n("transform.scale"));
    ImGui::SameLine(80);
    draw_vec3_control("Scale", scl, 0.01f, 1.0f);
    if (scl[0] != t->scale.x || scl[1] != t->scale.y || scl[2] != t->scale.z) {
        t->scale.x = scl[0]; t->scale.y = scl[1]; t->scale.z = scl[2];
    }
}

static void draw_comp_light(JceScene *scene, JceEntity e, uint32_t flags)
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
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Intensity", jce_editor_i18n("light.intensity"));
    if (ImGui::DragFloat(lbl, &intensity, 0.1f, 0.0f, 100.0f)) {
        if (light_type == 0)      { jce_scene_get_dir_light(scene, e)->intensity   = intensity; }
        else if (light_type == 1) { jce_scene_get_point_light(scene, e)->intensity = intensity; }
        else                      { jce_scene_get_spot_light(scene, e)->intensity  = intensity; }
    }
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
        if (ImGui::DragFloat("Inner Cone###InnerCone", &inner_deg, 0.5f, 0.0f, 89.0f))
            l->inner_cone_cos = cosf(inner_deg * JCE_DEG2RAD);
        insp_track_edit();
        if (ImGui::DragFloat("Outer Cone###OuterCone", &outer_deg, 0.5f, 0.0f, 90.0f))
            l->outer_cone_cos = cosf(outer_deg * JCE_DEG2RAD);
        insp_track_edit();
        /* Clamp: outer must be >= inner (i.e. outer cos <= inner cos). */
        if (l->outer_cone_cos > l->inner_cone_cos)
            l->outer_cone_cos = l->inner_cone_cos;
    }

    if (light_type == 0) {
        JceDirectionalLight *l = jce_scene_get_dir_light(scene, e);
        if (ImGui::Checkbox("Casts Shadow###CastsShadow", &l->casts_shadow))
            insp_undo_bool(&l->casts_shadow);
    }
}

static void draw_comp_camera(JceCameraComponent *cam)
{
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

    if (ImGui::Checkbox("Primary###CamPrimary", &cam->is_primary))
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
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("viewer.metallic"), &mr->metallic, 0.01f, 0.0f, 1.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("viewer.roughness"), &mr->roughness, 0.01f, 0.0f, 1.0f);
        insp_track_edit();
        ImGui::ColorEdit3(jce_editor_i18n("inspector.emissive"), mr->emissive);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.normalScale"), &mr->normal_scale, 0.01f, 0.0f, 4.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.aoStrength"), &mr->ao_strength, 0.01f, 0.0f, 2.0f);
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
        snprintf(lbl, sizeof(lbl), "%s##tex", jce_editor_i18n("inspector.texture.emissive"));
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

    snprintf(lbl, sizeof(lbl), "%s##skel", jce_editor_i18n("timeline.speed"));
    ImGui::DragFloat(lbl, &skel->speed, 0.01f, 0.01f, 10.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##skel", jce_editor_i18n("timeline.loop"));
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

    snprintf(lbl, sizeof(lbl), "%s##skel",
             skel->playing ? jce_editor_i18n("toolbar.stop")
                           : jce_editor_i18n("toolbar.play"));
    if (ImGui::Button(lbl))
        skel->playing = !skel->playing;
}

static void draw_comp_rigidbody(JceRigidBodyComponent *rb)
{
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s##rbMass", jce_editor_i18n("rigidbody.mass"));
    ImGui::DragFloat(lbl, &rb->mass, 0.1f, 0.0f, 10000.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##rbDrag", jce_editor_i18n("rigidbody.drag"));
    ImGui::DragFloat(lbl, &rb->drag, 0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##rbAngularDrag", jce_editor_i18n("rigidbody.angularDrag"));
    ImGui::DragFloat(lbl, &rb->angular_drag, 0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##rbUseGravity", jce_editor_i18n("rigidbody.useGravity"));
    if (ImGui::Checkbox(lbl, &rb->use_gravity))
        insp_undo_bool(&rb->use_gravity);
    snprintf(lbl, sizeof(lbl), "%s##rbIsKinematic", jce_editor_i18n("rigidbody.isKinematic"));
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
    snprintf(lbl, sizeof(lbl), "%s##box", jce_editor_i18n("collider.isTrigger"));
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
    snprintf(lbl, sizeof(lbl), "%s##sphere", jce_editor_i18n("collider.isTrigger"));
    if (ImGui::Checkbox(lbl, &sc->is_trigger))
        insp_undo_bool(&sc->is_trigger);
}

static void draw_comp_character_controller(JceCharacterControllerComponent *cc)
{
    char lbl[256];
    ImGui::DragFloat(jce_editor_i18n("collider.height"), &cc->height, 0.1f, 0.1f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##cc", jce_editor_i18n("collider.radius"));
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
    snprintf(lbl, sizeof(lbl), "%s##audio", jce_editor_i18n("audioSource.loop"));
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

/* ── Component header / settings popup helper ─────────────────────── */

/* Returns true if the component's body should be drawn this frame.
 * Updates sidecar.expanded_flags fold state.  Handles the "..." popup
 * with a Remove menu (disabled when not removable, e.g. Transform). */
static bool comp_section_begin(uint32_t entity_id,
                               EditorEntitySidecar &sidecar,
                               uint32_t flag,
                               const char *display_name,
                               bool removable)
{
    ImGui::PushID((int)flag);

    bool was_open = (sidecar.expanded_flags & flag) != 0;
    int tn_flags = ImGuiTreeNodeFlags_AllowOverlap |
                   (was_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);

    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    bool open = ImGui::CollapsingHeader(display_name, tn_flags);
    if (open) sidecar.expanded_flags |= flag;
    else      sidecar.expanded_flags &= ~flag;

    float header_w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(header_w - 20);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
        if (ImGui::MenuItem(jce_editor_i18n("transform.reset")))
            jce_editor_console_log("Reset %s (stub)", display_name);
        if (!removable) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent"), NULL, false, false);
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            if (ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent")))
                jce_state_remove_component(entity_id, flag);
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();

    return open;
}

static void comp_section_end(void)
{
    ImGui::Spacing();
    ImGui::PopID();
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
    uint32_t flag;
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
    { JCE_COMP_FLAG_BOX_COLLIDER,         false, true  },
    { JCE_COMP_FLAG_SPHERE_COLLIDER,      false, true  },
    { JCE_COMP_FLAG_CHARACTER_CONTROLLER, false, false },
    { JCE_COMP_FLAG_AUDIO_SOURCE,         false, false },
    { JCE_COMP_FLAG_SCRIPT,               false, false },
    { JCE_COMP_FLAG_CONSTRAINT,           false, false },
};

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_inspector_content(void)
{
    ensure_init();
    char lbl[128];

    JceScene *scene = jce_state_get_scene();

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

        for (int i = 0; i < sel_count && i < 20; i++) {
            const char *nm = jce_state_entity_name(sel_ids[i]);
            if (nm) ImGui::BulletText("%s", nm);
        }
        if (sel_count > 20)
            ImGui::Text("... %d %s", sel_count - 20, jce_editor_i18n("inspector.andMore"));

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
        if (ImGui::Combo(_lbl, &tag_color,
                          "None\0Red\0Orange\0Yellow\0Green\0Blue\0Purple\0Gray\0"))
            jce_state_set_entity_tag_color(focused, (JceTagColor)tag_color);
    }
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(-1);
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###tag", jce_editor_i18n("inspector.tag"));
        bool tag_committed = false;
        if (ImGui::InputTextWithHint(_lbl, jce_editor_i18n("inspector.tag"), s_insp.tag_buf,
                                      sizeof(s_insp.tag_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            jce_state_set_entity_tag(focused, s_insp.tag_buf);
            tag_committed = true;
        }
        if (!tag_committed && ImGui::IsItemDeactivatedAfterEdit())
            jce_state_set_entity_tag(focused, s_insp.tag_buf);
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    if (!ent_enabled) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.entityDisabled"));
        ImGui::Spacing();
    }

    ImGui::BeginDisabled(!ent_enabled);

    /* ── Components ───────────────────────────────────────────────── */
    JceEntity ecs_e = jce_state_to_ecs_entity(focused);
    uint32_t flags = jce_scene_get_component_flags(scene, ecs_e);
    EditorEntitySidecar &sidecar = g_entity_sidecar[focused];

    if (flags & JCE_COMP_FLAG_TRANSFORM) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_TRANSFORM,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_TRANSFORM), false))
            draw_comp_transform(focused, jce_scene_get_transform(scene, ecs_e));
        comp_section_end();
    }

    /* Light: unified section (one of dir/point/spot). */
    uint32_t light_mask = JCE_COMP_FLAG_DIR_LIGHT | JCE_COMP_FLAG_POINT_LIGHT |
                          JCE_COMP_FLAG_SPOT_LIGHT;
    if (flags & light_mask) {
        /* Pick whichever flag is present for the section's identity / fold state. */
        uint32_t light_flag = (flags & JCE_COMP_FLAG_DIR_LIGHT)   ? JCE_COMP_FLAG_DIR_LIGHT
                            : (flags & JCE_COMP_FLAG_POINT_LIGHT) ? JCE_COMP_FLAG_POINT_LIGHT
                                                                  : JCE_COMP_FLAG_SPOT_LIGHT;
        if (comp_section_begin(focused, sidecar, light_flag, "Light", true))
            draw_comp_light(scene, ecs_e, flags);
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_CAMERA) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_CAMERA,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_CAMERA), true))
            draw_comp_camera(jce_scene_get_camera(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_MESH_RENDERER) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_MESH_RENDERER,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_MESH_RENDERER), true))
            draw_comp_mesh_renderer(jce_scene_get_mesh_renderer(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_SPRITE_RENDERER) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_SPRITE_RENDERER,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_SPRITE_RENDERER), true))
            draw_comp_sprite_renderer(jce_scene_get_sprite_renderer(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_ANIMATOR) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_ANIMATOR,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_ANIMATOR), true))
            draw_comp_animator(jce_scene_get_animator(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_SKELETAL_ANIMATOR) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_SKELETAL_ANIMATOR,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_SKELETAL_ANIMATOR), true))
            draw_comp_skeletal_animator(jce_scene_get_skeletal_animator(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_RIGIDBODY) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_RIGIDBODY,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_RIGIDBODY), true))
            draw_comp_rigidbody(jce_scene_get_rigidbody(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_BOX_COLLIDER) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_BOX_COLLIDER,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_BOX_COLLIDER), true))
            draw_comp_box_collider(jce_scene_get_box_collider(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_SPHERE_COLLIDER) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_SPHERE_COLLIDER,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_SPHERE_COLLIDER), true))
            draw_comp_sphere_collider(jce_scene_get_sphere_collider(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_CHARACTER_CONTROLLER) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_CHARACTER_CONTROLLER,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_CHARACTER_CONTROLLER), true))
            draw_comp_character_controller(jce_scene_get_character_controller(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_AUDIO_SOURCE) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_AUDIO_SOURCE,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_AUDIO_SOURCE), true))
            draw_comp_audio_source(jce_scene_get_audio_source(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_SCRIPT) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_SCRIPT,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_SCRIPT), true))
            draw_comp_script(jce_scene_get_script(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_SKYBOX) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_SKYBOX,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_SKYBOX), true))
            draw_comp_skybox(jce_scene_get_skybox(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_SPRITE_ANIMATOR) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_SPRITE_ANIMATOR,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_SPRITE_ANIMATOR), true))
            draw_comp_sprite_animator(jce_scene_get_sprite_animator(scene, ecs_e));
        comp_section_end();
    }

    if (flags & JCE_COMP_FLAG_CONSTRAINT) {
        if (comp_section_begin(focused, sidecar, JCE_COMP_FLAG_CONSTRAINT,
                                jce_comp_flag_display_name(JCE_COMP_FLAG_CONSTRAINT), true))
            draw_comp_constraint(jce_scene_get_constraint(scene, ecs_e));
        comp_section_end();
    }

    /* ── Add Component button ─────────────────────────────────────── */
    ImGui::Spacing();
    float btn_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(jce_editor_i18n("inspector.addComponent"), ImVec2(btn_w, 0)))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup")) {
        const int n_opts = (int)(sizeof(s_add_options) / sizeof(s_add_options[0]));
        for (int i = 0; i < n_opts; i++) {
            const AddCompOption &opt = s_add_options[i];
            if (flags & opt.flag) continue;
            /* Hide light add-options when any light is already present. */
            if (opt.is_light && (flags & light_mask)) continue;
            const char *name = jce_comp_flag_display_name(opt.flag);
            if (!name) continue;
            if (ImGui::MenuItem(name))
                jce_state_add_component(focused, opt.flag);
        }
        ImGui::EndPopup();
    }

    ImGui::EndDisabled();
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_inspector(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###Inspector", jce_editor_i18n("Inspector"));
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
        for (int i = 0; i < n; i++)
            jce_state_delete_entity(ids[i]);
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
