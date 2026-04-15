/*
 * jce_panel_inspector.cpp  Inspector panel (entity properties).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "scene/jce_model_loader_assimp.h"
#include "scene/jce_editor_scene_render.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <jce/graphics/jce_pbr_material.h>
#include <jce/graphics/jce_model.h>
#include <jce/animation/jce_animation.h>
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

/* Called by hierarchy when selection changes. */
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

/* Call after continuous widgets (DragFloat, ColorEdit, InputText, etc.).
 * A batch edit is opened when the widget is activated and closed when
 * the widget is deactivated, giving one undo entry per user gesture. */
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

/* Wrap an instant bool toggle (Checkbox) with undo.  ImGui has already
 * flipped *value by the time this is called, so we temporarily restore
 * the old value, capture the snapshot, then re-apply. */
static void insp_undo_bool(bool *value)
{
    bool now = *value;
    *value = !now;
    jce_state_begin_batch_edit();
    *value = now;
    jce_state_end_batch_edit();
}

/* Wrap an instant int change (Combo) with undo. */
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

    /* X */
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

    /* Y */
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

    /* Z */
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

/* Returns true if the path has a known 3D mesh extension. */
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

/* Accept a mesh file drop: sets mesh_path, mesh_shape=0, and
 * extracts material/texture info from the model into the component. */
static void accept_mesh_drop_with_material(JceComponentInfo *comp)
{
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
            const char *path = (const char *)payload->Data;
            auto &mr = comp->data.mesh_renderer;
            snprintf(mr.mesh_path, sizeof(mr.mesh_path), "%s", path);
            mr.mesh_shape = 0; /* custom mesh */

            if (is_mesh_ext(path)) {
                JceEditorMaterialInfo mat = {};
                if (jce_editor_model_extract_material(path, &mat)) {
                    if (mat.albedo_tex[0])
                        snprintf(mr.albedo_tex, sizeof(mr.albedo_tex), "%s", mat.albedo_tex);
                    if (mat.mr_tex[0])
                        snprintf(mr.mr_tex, sizeof(mr.mr_tex), "%s", mat.mr_tex);
                    if (mat.normal_tex[0])
                        snprintf(mr.normal_tex, sizeof(mr.normal_tex), "%s", mat.normal_tex);
                    if (mat.ao_tex[0])
                        snprintf(mr.ao_tex, sizeof(mr.ao_tex), "%s", mat.ao_tex);
                    if (mat.emissive_tex[0])
                        snprintf(mr.emissive_tex, sizeof(mr.emissive_tex), "%s", mat.emissive_tex);

                    mr.base_color[0] = mat.base_color[0];
                    mr.base_color[1] = mat.base_color[1];
                    mr.base_color[2] = mat.base_color[2];
                    mr.base_color[3] = mat.base_color[3];
                    mr.metallic       = mat.metallic;
                    mr.roughness      = mat.roughness;
                    mr.emissive[0]    = mat.emissive[0];
                    mr.emissive[1]    = mat.emissive[1];
                    mr.emissive[2]    = mat.emissive[2];
                    mr.normal_scale   = mat.normal_scale;
                    mr.ao_strength    = mat.ao_strength;
                    mr.alpha_mode     = mat.alpha_mode;
                    mr.alpha_cutoff   = mat.alpha_cutoff;
                    mr.double_sided   = mat.double_sided;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

/* ── Per-component draw helpers ───────────────────────────────────── */

static void draw_comp_transform(JceComponentInfo *comp)
{
    ImGui::Text("%s", jce_editor_i18n("transform.position"));
    ImGui::SameLine(80);
    draw_vec3_control("Position", comp->data.transform.pos);
    ImGui::Text("%s", jce_editor_i18n("transform.rotation"));
    ImGui::SameLine(80);
    draw_vec3_control("Rotation", comp->data.transform.rot, 1.0f);
    ImGui::Text("%s", jce_editor_i18n("transform.scale"));
    ImGui::SameLine(80);
    draw_vec3_control("Scale", comp->data.transform.scale, 0.01f, 1.0f);
}

static void draw_comp_light(JceComponentInfo *comp)
{
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s###Color", jce_editor_i18n("light.color"));
    ImGui::ColorEdit4(lbl, comp->data.light.color);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Intensity", jce_editor_i18n("light.intensity"));
    ImGui::DragFloat(lbl, &comp->data.light.intensity, 0.1f, 0.0f, 100.0f);
    insp_track_edit();

    const char *light_types[] = {
        jce_editor_i18n("light.directional"),
        jce_editor_i18n("light.point"),
        jce_editor_i18n("light.spot")
    };
    int prev_type = comp->data.light.type;
    snprintf(lbl, sizeof(lbl), "%s###Type", jce_editor_i18n("light.type"));
    ImGui::Combo(lbl, &comp->data.light.type, light_types, 3);
    if (comp->data.light.type != prev_type)
        insp_undo_int(&comp->data.light.type, prev_type);
}

static void draw_comp_camera(JceComponentInfo *comp)
{
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s###FOV", jce_editor_i18n("camera.fov"));
    ImGui::DragFloat(lbl, &comp->data.camera.fov, 1.0f, 1.0f, 179.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Near", jce_editor_i18n("camera.nearClip"));
    ImGui::DragFloat(lbl, &comp->data.camera.near_clip, 0.01f, 0.001f, 100.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Far", jce_editor_i18n("camera.farClip"));
    ImGui::DragFloat(lbl, &comp->data.camera.far_clip, 1.0f, 1.0f, 100000.0f);
    insp_track_edit();

    snprintf(lbl, sizeof(lbl), "%s###Orthographic", jce_editor_i18n("camera.orthographic"));
    if (ImGui::Checkbox(lbl, &comp->data.camera.ortho))
        insp_undo_bool(&comp->data.camera.ortho);
}

/* Returns true if the path has a .mat.json extension. */
static bool is_mat_json(const char *path)
{
    if (!path) return false;
    size_t len = strlen(path);
    return (len >= 9 && strcmp(path + len - 9, ".mat.json") == 0);
}

/* Load PBR properties and texture paths from the .mat.json file
 * referenced by the component's material_path into its fields. */
static void load_material_into_component(JceComponentInfo *comp)
{
    auto &mr = comp->data.mesh_renderer;
    if (!is_mat_json(mr.material_path)) return;

    JcePbrMaterial mat;
    char tex_paths[5][256] = {};
    if (!jce_pbr_material_load_json(mr.material_path, &mat, tex_paths))
        return;

    mr.base_color[0] = mat.base_color_factor[0];
    mr.base_color[1] = mat.base_color_factor[1];
    mr.base_color[2] = mat.base_color_factor[2];
    mr.base_color[3] = mat.base_color_factor[3];
    mr.metallic       = mat.metallic_factor;
    mr.roughness      = mat.roughness_factor;
    mr.emissive[0]    = mat.emissive_factor[0];
    mr.emissive[1]    = mat.emissive_factor[1];
    mr.emissive[2]    = mat.emissive_factor[2];
    mr.normal_scale   = mat.normal_scale;
    mr.ao_strength    = mat.ao_strength;
    mr.alpha_mode     = (int)mat.alpha_mode;
    mr.alpha_cutoff   = mat.alpha_cutoff;
    mr.double_sided   = mat.double_sided;

    if (tex_paths[0][0]) snprintf(mr.albedo_tex,   sizeof(mr.albedo_tex),   "%s", tex_paths[0]);
    if (tex_paths[1][0]) snprintf(mr.mr_tex,       sizeof(mr.mr_tex),       "%s", tex_paths[1]);
    if (tex_paths[2][0]) snprintf(mr.normal_tex,   sizeof(mr.normal_tex),   "%s", tex_paths[2]);
    if (tex_paths[3][0]) snprintf(mr.ao_tex,       sizeof(mr.ao_tex),       "%s", tex_paths[3]);
    if (tex_paths[4][0]) snprintf(mr.emissive_tex, sizeof(mr.emissive_tex), "%s", tex_paths[4]);
}

/* Accept a .mat.json drop on the Materials field: stores the path
 * AND loads PBR properties + texture paths from the file. */
static void accept_material_drop(JceComponentInfo *comp)
{
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
            const char *path = (const char *)payload->Data;
            auto &mr = comp->data.mesh_renderer;
            jce_state_begin_batch_edit();
            snprintf(mr.material_path, sizeof(mr.material_path), "%s", path);
            load_material_into_component(comp);
            jce_state_end_batch_edit();
        }
        ImGui::EndDragDropTarget();
    }
}

static void draw_comp_mesh_renderer(JceComponentInfo *comp)
{
    char lbl[256];

    /* Shape selector (procedural mesh fallback). */
    const char *shape_names[] = {
        jce_editor_i18n("menu.gameObject.createCube"),
        jce_editor_i18n("menu.gameObject.createSphere"),
        jce_editor_i18n("menu.gameObject.createPlane"),
        jce_editor_i18n("inspector.shape.capsule"),
        jce_editor_i18n("menu.gameObject.createCylinder")
    };
    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s",
                       jce_editor_i18n("inspector.shape"));
    ImGui::SameLine();
    int prev_shape = comp->data.mesh_renderer.mesh_shape;
    ImGui::Combo("##mesh_shape", &comp->data.mesh_renderer.mesh_shape,
                 shape_names, 5);
    if (comp->data.mesh_renderer.mesh_shape != prev_shape)
        insp_undo_int(&comp->data.mesh_renderer.mesh_shape, prev_shape);

    /* Mesh / material file paths. */
    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.mesh"));
    ImGui::SameLine();
    ImGui::InputText("##mesh_path", comp->data.mesh_renderer.mesh_path,
                     sizeof(comp->data.mesh_renderer.mesh_path));
    insp_track_edit();
    accept_mesh_drop_with_material(comp);
    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.materials"));
    ImGui::SameLine();
    ImGui::InputText("##mat_path", comp->data.mesh_renderer.material_path,
                     sizeof(comp->data.mesh_renderer.material_path));
    if (ImGui::IsItemDeactivatedAfterEdit() && is_mat_json(comp->data.mesh_renderer.material_path)) {
        jce_state_begin_batch_edit();
        load_material_into_component(comp);
        jce_state_end_batch_edit();
    }
    insp_track_edit();
    accept_material_drop(comp);
    /* Reload button: re-read .mat.json into PBR fields + texture slots. */
    if (is_mat_json(comp->data.mesh_renderer.material_path)) {
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("codeViewer.reload"))) {
            jce_state_begin_batch_edit();
            load_material_into_component(comp);
            jce_state_end_batch_edit();
        }
    }

    /* PBR Parameters */
    if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.pbrMaterial"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::ColorEdit4(jce_editor_i18n("inspector.baseColor"), comp->data.mesh_renderer.base_color);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("viewer.metallic"), &comp->data.mesh_renderer.metallic,
                         0.01f, 0.0f, 1.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("viewer.roughness"), &comp->data.mesh_renderer.roughness,
                         0.01f, 0.0f, 1.0f);
        insp_track_edit();
        ImGui::ColorEdit3(jce_editor_i18n("inspector.emissive"), comp->data.mesh_renderer.emissive);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.normalScale"), &comp->data.mesh_renderer.normal_scale,
                         0.01f, 0.0f, 4.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.aoStrength"), &comp->data.mesh_renderer.ao_strength,
                         0.01f, 0.0f, 2.0f);
        insp_track_edit();
        const char *alpha_modes[] = {
            jce_editor_i18n("inspector.alphaMode.opaque"),
            jce_editor_i18n("inspector.alphaMode.mask"),
            jce_editor_i18n("inspector.alphaMode.blend")
        };
        int prev_alpha = comp->data.mesh_renderer.alpha_mode;
        ImGui::Combo(jce_editor_i18n("inspector.alphaMode"), &comp->data.mesh_renderer.alpha_mode,
                     alpha_modes, 3);
        if (comp->data.mesh_renderer.alpha_mode != prev_alpha)
            insp_undo_int(&comp->data.mesh_renderer.alpha_mode, prev_alpha);
        if (comp->data.mesh_renderer.alpha_mode == 1) {
            ImGui::DragFloat(jce_editor_i18n("inspector.alphaCutoff"),
                             &comp->data.mesh_renderer.alpha_cutoff,
                             0.01f, 0.0f, 1.0f);
            insp_track_edit();
        }
        if (ImGui::Checkbox(jce_editor_i18n("inspector.doubleSided"), &comp->data.mesh_renderer.double_sided))
            insp_undo_bool(&comp->data.mesh_renderer.double_sided);
        ImGui::TreePop();
    }

    /* Texture Slots */
    if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.textures"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputText(jce_editor_i18n("inspector.texture.albedo"), comp->data.mesh_renderer.albedo_tex, 128);
        insp_track_edit();
        accept_asset_drop(comp->data.mesh_renderer.albedo_tex, 128);
        ImGui::InputText(jce_editor_i18n("inspector.texture.metalRough"), comp->data.mesh_renderer.mr_tex, 128);
        insp_track_edit();
        accept_asset_drop(comp->data.mesh_renderer.mr_tex, 128);
        ImGui::InputText(jce_editor_i18n("inspector.texture.normal"), comp->data.mesh_renderer.normal_tex, 128);
        insp_track_edit();
        accept_asset_drop(comp->data.mesh_renderer.normal_tex, 128);
        ImGui::InputText(jce_editor_i18n("inspector.texture.ao"), comp->data.mesh_renderer.ao_tex, 128);
        insp_track_edit();
        accept_asset_drop(comp->data.mesh_renderer.ao_tex, 128);
        snprintf(lbl, sizeof(lbl), "%s##tex", jce_editor_i18n("inspector.texture.emissive"));
        ImGui::InputText(lbl, comp->data.mesh_renderer.emissive_tex, 128);
        insp_track_edit();
        accept_asset_drop(comp->data.mesh_renderer.emissive_tex, 128);
        ImGui::TreePop();
    }
}

static void draw_comp_sprite_renderer(JceComponentInfo *comp)
{
    ImGui::InputText(jce_editor_i18n("spriteRenderer.sprite"), comp->data.sprite_renderer.sprite_path, 128);
    insp_track_edit();
    accept_asset_drop(comp->data.sprite_renderer.sprite_path, 128);
    ImGui::ColorEdit4(jce_editor_i18n("spriteRenderer.color"), comp->data.sprite_renderer.color);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("spriteRenderer.flipX"), &comp->data.sprite_renderer.flip_x))
        insp_undo_bool(&comp->data.sprite_renderer.flip_x);
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n("spriteRenderer.flipY"), &comp->data.sprite_renderer.flip_y))
        insp_undo_bool(&comp->data.sprite_renderer.flip_y);
    ImGui::DragInt(jce_editor_i18n("spriteRenderer.orderInLayer"), &comp->data.sprite_renderer.sorting_order);
    insp_track_edit();
}

static void draw_comp_animator(JceComponentInfo *comp)
{
    if (comp->data.animator.speed <= 0.0f)
        comp->data.animator.speed = 1.0f;

    ImGui::InputText(jce_editor_i18n("timeline.clip"), comp->data.animator.clip_name, 64);
    insp_track_edit();
    accept_asset_drop(comp->data.animator.clip_name, 64);
    ImGui::DragFloat(jce_editor_i18n("timeline.speed"), &comp->data.animator.speed,
                     0.01f, 0.01f, 10.0f);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("timeline.loop"), &comp->data.animator.loop))
        insp_undo_bool(&comp->data.animator.loop);
    if (ImGui::Button(comp->data.animator.playing
                      ? jce_editor_i18n("toolbar.stop")
                      : jce_editor_i18n("toolbar.play")))
        comp->data.animator.playing = !comp->data.animator.playing;
}

static void draw_comp_skeletal_animator(JceComponentInfo *comp)
{
    char lbl[256];

    if (comp->data.skeletal_animator.speed <= 0.0f)
        comp->data.skeletal_animator.speed = 1.0f;

    ImGui::InputText(jce_editor_i18n("inspector.skeleton"), comp->data.skeletal_animator.skeleton_path, 128);
    insp_track_edit();
    accept_asset_drop(comp->data.skeletal_animator.skeleton_path, 128);

    /* Auto-populate clip list from model if path is set but clips empty. */
    if (comp->data.skeletal_animator.skeleton_path[0] &&
        comp->data.skeletal_animator.clip_count == 0) {
        JceModel *mdl = jce_editor_scene_get_model(
            comp->data.skeletal_animator.skeleton_path, 0);
        if (mdl) {
            uint32_t n = jce_model_anim_count(mdl);
            if (n > 8) n = 8;
            comp->data.skeletal_animator.clip_count = (int)n;
            for (uint32_t ci = 0; ci < n; ++ci) {
                JceAnimClip *clip = jce_model_get_anim(mdl, ci);
                const char *name = clip ? jce_anim_clip_name(clip) : "clip";
                snprintf(comp->data.skeletal_animator.clip_names[ci],
                         sizeof(comp->data.skeletal_animator.clip_names[ci]),
                         "%s", name ? name : "clip");
            }
        }
    }

    snprintf(lbl, sizeof(lbl), "%s##skel", jce_editor_i18n("timeline.speed"));
    ImGui::DragFloat(lbl, &comp->data.skeletal_animator.speed,
                     0.01f, 0.01f, 10.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##skel", jce_editor_i18n("timeline.loop"));
    if (ImGui::Checkbox(lbl, &comp->data.skeletal_animator.loop))
        insp_undo_bool(&comp->data.skeletal_animator.loop);

    int clip_count = comp->data.skeletal_animator.clip_count;
    if (clip_count < 0) clip_count = 0;
    if (clip_count > 8) clip_count = 8;
    if (comp->data.skeletal_animator.active_clip < 0)
        comp->data.skeletal_animator.active_clip = 0;
    if (clip_count > 0 && comp->data.skeletal_animator.active_clip >= clip_count)
        comp->data.skeletal_animator.active_clip = clip_count - 1;

    if (clip_count > 0) {
        int prev_clip = comp->data.skeletal_animator.active_clip;
        ImGui::Combo(jce_editor_i18n("inspector.activeClip"), &comp->data.skeletal_animator.active_clip,
            [](void *data, int idx) -> const char* {
                auto *sa = (decltype(comp->data.skeletal_animator)*)data;
                return sa->clip_names[idx]; },
            &comp->data.skeletal_animator,
            clip_count);
        if (comp->data.skeletal_animator.active_clip != prev_clip)
            insp_undo_int(&comp->data.skeletal_animator.active_clip, prev_clip);
    }

    /* Progress bar showing animation time. */
    if (comp->data.skeletal_animator.skeleton_path[0]) {
        JceAnimPlayer *pl = jce_editor_scene_get_anim_player(
            comp->data.skeletal_animator.skeleton_path, 0);
        if (pl) {
            float t = jce_anim_player_get_time(pl);
            JceModel *mdl = jce_editor_scene_get_model(
                comp->data.skeletal_animator.skeleton_path, 0);
            float dur = 1.0f;
            if (mdl) {
                JceAnimClip *clip = jce_model_get_anim(mdl,
                    (uint32_t)comp->data.skeletal_animator.active_clip);
                if (clip) dur = jce_anim_clip_duration(clip);
            }
            float frac = (dur > 0.0f) ? (t / dur) : 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            snprintf(lbl, sizeof(lbl), "%.2fs / %.2fs", t, dur);
            ImGui::ProgressBar(frac, ImVec2(-1, 0), lbl);
        }
    }

    snprintf(lbl, sizeof(lbl), "%s##skel",
             comp->data.skeletal_animator.playing
                ? jce_editor_i18n("toolbar.stop")
                : jce_editor_i18n("toolbar.play"));
    if (ImGui::Button(lbl))
        comp->data.skeletal_animator.playing = !comp->data.skeletal_animator.playing;
}

static void draw_comp_rigidbody(JceComponentInfo *comp)
{
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s##rbMass", jce_editor_i18n("rigidbody.mass"));
    ImGui::DragFloat(lbl, &comp->data.rigidbody.mass, 0.1f, 0.0f, 10000.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##rbDrag", jce_editor_i18n("rigidbody.drag"));
    ImGui::DragFloat(lbl, &comp->data.rigidbody.drag, 0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##rbAngularDrag", jce_editor_i18n("rigidbody.angularDrag"));
    ImGui::DragFloat(lbl, &comp->data.rigidbody.angular_drag,
                     0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##rbUseGravity", jce_editor_i18n("rigidbody.useGravity"));
    if (ImGui::Checkbox(lbl, &comp->data.rigidbody.use_gravity))
        insp_undo_bool(&comp->data.rigidbody.use_gravity);
    snprintf(lbl, sizeof(lbl), "%s##rbIsKinematic", jce_editor_i18n("rigidbody.isKinematic"));
    if (ImGui::Checkbox(lbl, &comp->data.rigidbody.is_kinematic))
        insp_undo_bool(&comp->data.rigidbody.is_kinematic);
}

static void draw_comp_box_collider(JceComponentInfo *comp)
{
    char lbl[256];
    ImGui::Text("%s", jce_editor_i18n("collider.center"));
    ImGui::SameLine(80);
    draw_vec3_control("BoxCenter", comp->data.box_collider.center);
    ImGui::Text("%s", jce_editor_i18n("collider.size"));
    ImGui::SameLine(80);
    draw_vec3_control("BoxSize", comp->data.box_collider.size, 0.01f, 1.0f);
    snprintf(lbl, sizeof(lbl), "%s##box", jce_editor_i18n("collider.isTrigger"));
    if (ImGui::Checkbox(lbl, &comp->data.box_collider.is_trigger))
        insp_undo_bool(&comp->data.box_collider.is_trigger);
}

static void draw_comp_sphere_collider(JceComponentInfo *comp)
{
    char lbl[256];
    ImGui::Text("%s", jce_editor_i18n("collider.center"));
    ImGui::SameLine(80);
    draw_vec3_control("SphereCenter", comp->data.sphere_collider.center);
    ImGui::DragFloat(jce_editor_i18n("collider.radius"), &comp->data.sphere_collider.radius,
                     0.01f, 0.001f, 1000.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##sphere", jce_editor_i18n("collider.isTrigger"));
    if (ImGui::Checkbox(lbl, &comp->data.sphere_collider.is_trigger))
        insp_undo_bool(&comp->data.sphere_collider.is_trigger);
}

static void draw_comp_character_controller(JceComponentInfo *comp)
{
    char lbl[256];
    ImGui::DragFloat(jce_editor_i18n("collider.height"), &comp->data.character_controller.height,
                     0.1f, 0.1f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##cc", jce_editor_i18n("collider.radius"));
    ImGui::DragFloat(lbl, &comp->data.character_controller.radius,
                     0.01f, 0.01f, 50.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.stepOffset"), &comp->data.character_controller.step_offset,
                     0.01f, 0.0f, 10.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.slopeLimit"), &comp->data.character_controller.slope_limit,
                     1.0f, 0.0f, 90.0f);
    insp_track_edit();
}

static void draw_comp_audio_source(JceComponentInfo *comp)
{
    char lbl[256];
    ImGui::InputText(jce_editor_i18n("audioSource.clip"), comp->data.audio_source.clip_path, 128);
    insp_track_edit();
    accept_asset_drop(comp->data.audio_source.clip_path, 128);
    ImGui::DragFloat(jce_editor_i18n("audioSource.volume"), &comp->data.audio_source.volume,
                     0.01f, 0.0f, 1.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("audioSource.pitch"), &comp->data.audio_source.pitch,
                     0.01f, 0.01f, 3.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.spatialBlend"), &comp->data.audio_source.spatial_blend,
                     0.01f, 0.0f, 1.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s##audio", jce_editor_i18n("audioSource.loop"));
    if (ImGui::Checkbox(lbl, &comp->data.audio_source.loop))
        insp_undo_bool(&comp->data.audio_source.loop);
    if (ImGui::Checkbox(jce_editor_i18n("audioSource.playOnAwake"), &comp->data.audio_source.play_on_awake))
        insp_undo_bool(&comp->data.audio_source.play_on_awake);
}

static void draw_comp_script(JceComponentInfo *comp)
{
    ImGui::InputText(jce_editor_i18n("inspector.script"), comp->data.script.script_path, 128);
    insp_track_edit();
    accept_asset_drop(comp->data.script.script_path, 128);
}

/* ── Component editor ─────────────────────────────────────────────── */

static void draw_component(JceComponentInfo *comp, uint32_t entity_id)
{
    const char *name = jce_component_type_name(comp->type);
    ImGui::PushID((int)comp->type);

    ImGui::PushStyleColor(ImGuiCol_Header, JCE_COLOR_INSP_HEADER);
    bool open = ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen |
                                               ImGuiTreeNodeFlags_AllowOverlap);

    float header_w = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(header_w - 20);
    if (ImGui::SmallButton("..."))
        ImGui::OpenPopup("ComponentSettings");

    if (ImGui::BeginPopup("ComponentSettings")) {
        if (ImGui::MenuItem(jce_editor_i18n("transform.reset")))
            jce_editor_console_log("Reset %s (stub)", name);
        if (comp->type == JCE_COMP_TRANSFORM) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent"),
                            NULL, false, false);
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            if (ImGui::MenuItem(jce_editor_i18n("inspector.removeComponent")))
                jce_state_remove_component(entity_id, comp->type);
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();

    if (open) {
        switch (comp->type) {
        case JCE_COMP_TRANSFORM:            draw_comp_transform(comp);            break;
        case JCE_COMP_LIGHT:                draw_comp_light(comp);                break;
        case JCE_COMP_CAMERA:               draw_comp_camera(comp);               break;
        case JCE_COMP_MESH_RENDERER:        draw_comp_mesh_renderer(comp);        break;
        case JCE_COMP_SPRITE_RENDERER:      draw_comp_sprite_renderer(comp);      break;
        case JCE_COMP_ANIMATOR:             draw_comp_animator(comp);             break;
        case JCE_COMP_SKELETAL_ANIMATOR:    draw_comp_skeletal_animator(comp);    break;
        case JCE_COMP_RIGIDBODY:            draw_comp_rigidbody(comp);            break;
        case JCE_COMP_BOX_COLLIDER:         draw_comp_box_collider(comp);         break;
        case JCE_COMP_SPHERE_COLLIDER:      draw_comp_sphere_collider(comp);      break;
        case JCE_COMP_CHARACTER_CONTROLLER: draw_comp_character_controller(comp); break;
        case JCE_COMP_AUDIO_SOURCE:         draw_comp_audio_source(comp);         break;
        case JCE_COMP_SCRIPT:               draw_comp_script(comp);               break;
        default:
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.propertiesNotImplemented"));
            break;
        }
        ImGui::Spacing();
    }

    ImGui::PopID();
}

/* ── Material file sync ───────────────────────────────────────────── */

void jce_editor_inspector_reload_material(const char *material_path)
{
    if (!material_path || material_path[0] == '\0') return;

    int total = jce_state_get_entity_count();
    for (int i = 0; i < total; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent) continue;

        int comp_count = 0;
        JceComponentInfo *comps = jce_state_get_entity_components(ent->id, &comp_count);
        for (int c = 0; c < comp_count; c++) {
            if (comps[c].type == JCE_COMP_MESH_RENDERER &&
                strcmp(comps[c].data.mesh_renderer.material_path, material_path) == 0) {
                load_material_into_component(&comps[c]);
            }
        }
    }
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_inspector_content(void)
{
    ensure_init();
    char lbl[128];

    /* ── Multi-entity selection header ─────────────────────────────── */
    int sel_count = 0;
    const uint32_t *sel_ids = jce_state_get_selection(&sel_count);

    if (sel_count > 1) {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                           "%d %s", sel_count, jce_editor_i18n("inspector.entitiesSelected"));
        ImGui::Separator();

        /* Shared enabled toggle — check if all are same state. */
        bool all_enabled = true, all_disabled = true;
        for (int i = 0; i < sel_count; i++) {
            JceEntityInfo *si = jce_state_get_entity(sel_ids[i]);
            if (si && si->enabled)  all_disabled = false;
            if (si && !si->enabled) all_enabled  = false;
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

        /* Shared tag color. */
        JceEntityInfo *first = jce_state_get_entity(sel_ids[0]);
        bool same_color = true;
        for (int i = 1; first && i < sel_count; i++) {
            JceEntityInfo *si = jce_state_get_entity(sel_ids[i]);
            if (si && si->tag_color != first->tag_color) { same_color = false; break; }
        }

        if (same_color && first) {
            int tag_color = (int)first->tag_color;
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

        /* List selected entities. */
        for (int i = 0; i < sel_count && i < 20; i++) {
            JceEntityInfo *si = jce_state_get_entity(sel_ids[i]);
            if (si) ImGui::BulletText("%s", si->name);
        }
        if (sel_count > 20)
            ImGui::Text("... %d %s", sel_count - 20, jce_editor_i18n("inspector.andMore"));

        ImGui::Separator();

        /* Bulk delete button. */
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        float btn_w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(jce_editor_i18n("inspector.deleteSelected"), ImVec2(btn_w, 0)))
            jce_editor_inspector_request_delete_confirm_many(sel_ids, sel_count);
        ImGui::PopStyleColor(2);

        return; /* multi-entity mode doesn't show individual components */
    }

    uint32_t focused = jce_state_get_focused();
    JceEntityInfo *e = focused ? jce_state_get_entity(focused) : NULL;

    if (!e) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.noSelection"));
        return;
    }

    /* Entity header: name + delete button */
    if (s_insp.needs_sync) {
        snprintf(s_insp.name_buf, sizeof(s_insp.name_buf), "%s", e->name);
        snprintf(s_insp.tag_buf, sizeof(s_insp.tag_buf), "%s", e->tag);
        s_insp.needs_sync = false;
    }

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 50);
    if (ImGui::InputText("##name", s_insp.name_buf, sizeof(s_insp.name_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        jce_state_rename_entity(e->id, s_insp.name_buf);
    ImGui::PopItemWidth();

    /* Delete button (red) — opens confirmation modal */
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("inspector.delete")))
        jce_editor_inspector_request_delete_confirm(e->id);
    ImGui::PopStyleColor(2);

    /* Row 2: [✓ Enabled]  [Tag Color ▼] */
    bool enabled = e->enabled;
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###enabled", jce_editor_i18n("inspector.enabled"));
        if (ImGui::Checkbox(_lbl, &enabled))
            jce_state_set_entity_enabled(e->id, enabled);
    }
    ImGui::SameLine();

    int tag_color = (int)e->tag_color;
    float combo_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushItemWidth(combo_w);
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###TagColor", jce_editor_i18n("inspector.tagColor"));
        if (ImGui::Combo(_lbl, &tag_color,
                          "None\0Red\0Orange\0Yellow\0Green\0Blue\0Purple\0Gray\0"))
            jce_state_set_entity_tag_color(e->id, (JceTagColor)tag_color);
    }
    ImGui::PopItemWidth();

    /* Row 3: Tag string input */
    ImGui::PushItemWidth(-1);
    {
        char _lbl[256];
        snprintf(_lbl, sizeof(_lbl), "%s###tag", jce_editor_i18n("inspector.tag"));
        bool tag_committed = false;
        if (ImGui::InputTextWithHint(_lbl, jce_editor_i18n("inspector.tag"), s_insp.tag_buf,
                                      sizeof(s_insp.tag_buf),
                                      ImGuiInputTextFlags_EnterReturnsTrue)) {
            jce_state_set_entity_tag(e->id, s_insp.tag_buf);
            tag_committed = true;
        }

        if (!tag_committed && ImGui::IsItemDeactivatedAfterEdit())
            jce_state_set_entity_tag(e->id, s_insp.tag_buf);
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    if (!e->enabled) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.entityDisabled"));
        ImGui::Spacing();
    }

    ImGui::BeginDisabled(!e->enabled);

    /* Components — use pointer-based access so DragFloat edits modify
     * the actual state directly (not a local copy). */
    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(e->id, &comp_count);

    if (comps) {
        for (int i = 0; i < comp_count; i++)
            draw_component(&comps[i], e->id);
    }

    /* Add Component button */
    ImGui::Spacing();
    float btn_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(jce_editor_i18n("inspector.addComponent"), ImVec2(btn_w, 0)))
        ImGui::OpenPopup("AddComponentPopup");

    if (ImGui::BeginPopup("AddComponentPopup")) {
        int popup_comp_count = 0;
        JceComponentInfo *popup_comps = jce_state_get_entity_components(e->id,
                                                                         &popup_comp_count);
        for (int t = 0; t < JCE_COMP_TYPE_COUNT; t++) {
            /* Skip components already present on this entity. */
            bool already_has = false;
            for (int ci = 0; ci < popup_comp_count; ci++) {
                if (popup_comps[ci].type == (JceComponentType)t) {
                    already_has = true;
                    break;
                }
            }
            if (already_has) continue;

            if (ImGui::MenuItem(jce_component_type_name((JceComponentType)t)))
                jce_state_add_component(e->id, (JceComponentType)t);
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

        /* Dialog window (dimmer is handled globally by jce_editor_layout). */
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(330, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowFocus();

        bool keep_open = s_insp.delete_requested;

        char title[256];
        snprintf(title, sizeof(title), "%s###ConfirmDeleteEntityDlg", jce_editor_i18n("dialog.confirmDelete"));
        if (!ImGui::Begin(title,
                                            &keep_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
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
        if (n > JCE_MAX_SELECTED)
            n = JCE_MAX_SELECTED;
        for (int i = 0; i < n; i++)
            ids[i] = s_insp.delete_entity_ids[i];

        if (n > 1)
            jce_state_begin_batch_edit();
        for (int i = 0; i < n; i++)
            jce_state_delete_entity(ids[i]);
        if (n > 1)
            jce_state_end_batch_edit();

        keep_open = false;
        ImGui::PopStyleColor(3);
        ImGui::End();
        s_insp.delete_requested = keep_open;
        if (!keep_open)
            s_insp.delete_entity_count = 0;
        return;
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("inspector.no"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        keep_open = false;
    }

    ImGui::End();
    s_insp.delete_requested = keep_open;
    if (!keep_open)
        s_insp.delete_entity_count = 0;
}
