/*
 * jce_panel_inspector.cpp  Inspector panel (entity properties).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

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
    if (ImGui::Button("X", btn_size)) values[0] = reset_value;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##X", &values[0], speed);
    ImGui::PopItemWidth();

    /* Y */
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Y);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.9f, 0.2f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.8f, 0.1f, 1.0f));
    if (ImGui::Button("Y", btn_size)) values[1] = reset_value;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Y", &values[1], speed);
    ImGui::PopItemWidth();

    /* Z */
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,        JCE_COLOR_INSP_VEC_Z);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.2f, 0.2f, 0.9f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.1f, 0.1f, 0.8f, 1.0f));
    if (ImGui::Button("Z", btn_size)) values[2] = reset_value;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    ImGui::PushItemWidth(width);
    ImGui::DragFloat("##Z", &values[2], speed);
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

/* ── Component editor ─────────────────────────────────────────────── */

static void draw_component(JceComponentInfo *comp, uint32_t entity_id)
{
    const char *name = jce_component_type_name(comp->type);
    char lbl[256];
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
        if (comp->type != JCE_COMP_TRANSFORM) {
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
        case JCE_COMP_TRANSFORM:
            ImGui::Text("%s", jce_editor_i18n("transform.position"));
            ImGui::SameLine(80);
            draw_vec3_control("Position", comp->data.transform.pos);
            ImGui::Text("%s", jce_editor_i18n("transform.rotation"));
            ImGui::SameLine(80);
            draw_vec3_control("Rotation", comp->data.transform.rot, 1.0f);
            ImGui::Text("%s", jce_editor_i18n("transform.scale"));
            ImGui::SameLine(80);
            draw_vec3_control("Scale", comp->data.transform.scale, 0.01f, 1.0f);
            break;

        case JCE_COMP_LIGHT:
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Color", jce_editor_i18n("light.color"));
                ImGui::ColorEdit4(_lbl, comp->data.light.color);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Intensity", jce_editor_i18n("light.intensity"));
                ImGui::DragFloat(_lbl, &comp->data.light.intensity, 0.1f, 0.0f, 100.0f);
            }
            {
                const char *light_types[] = {
                    jce_editor_i18n("light.directional"),
                    jce_editor_i18n("light.point"),
                    jce_editor_i18n("light.spot")
                };
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Type", jce_editor_i18n("light.type"));
                ImGui::Combo(_lbl, &comp->data.light.type, light_types, 3);
            }
            break;

        case JCE_COMP_CAMERA:
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###FOV", jce_editor_i18n("camera.fov"));
                ImGui::DragFloat(_lbl, &comp->data.camera.fov, 1.0f, 1.0f, 179.0f);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Near", jce_editor_i18n("camera.nearClip"));
                ImGui::DragFloat(_lbl, &comp->data.camera.near_clip, 0.01f, 0.001f, 100.0f);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Far", jce_editor_i18n("camera.farClip"));
                ImGui::DragFloat(_lbl, &comp->data.camera.far_clip, 1.0f, 1.0f, 100000.0f);
            }
            {
                char _lbl[256];
                snprintf(_lbl, sizeof(_lbl), "%s###Orthographic", jce_editor_i18n("camera.orthographic"));
                ImGui::Checkbox(_lbl, &comp->data.camera.ortho);
            }
            break;

        case JCE_COMP_MESH_RENDERER:
            /* Shape selector (procedural mesh fallback). */
            {
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
                ImGui::Combo("##mesh_shape", &comp->data.mesh_renderer.mesh_shape,
                             shape_names, 5);
            }
            /* Mesh / material file paths. */
            ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.mesh"));
            ImGui::SameLine();
            ImGui::InputText("##mesh_path", comp->data.mesh_renderer.mesh_path,
                             sizeof(comp->data.mesh_renderer.mesh_path));
            accept_asset_drop(comp->data.mesh_renderer.mesh_path,
                              sizeof(comp->data.mesh_renderer.mesh_path));
            ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.materials"));
            ImGui::SameLine();
            ImGui::InputText("##mat_path", comp->data.mesh_renderer.material_path,
                             sizeof(comp->data.mesh_renderer.material_path));
            accept_asset_drop(comp->data.mesh_renderer.material_path,
                              sizeof(comp->data.mesh_renderer.material_path));

            /* --- PBR Parameters --- */
            if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.pbrMaterial"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::ColorEdit4(jce_editor_i18n("inspector.baseColor"), comp->data.mesh_renderer.base_color);
                ImGui::DragFloat(jce_editor_i18n("viewer.metallic"), &comp->data.mesh_renderer.metallic,
                                 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat(jce_editor_i18n("viewer.roughness"), &comp->data.mesh_renderer.roughness,
                                 0.01f, 0.0f, 1.0f);
                ImGui::ColorEdit3(jce_editor_i18n("inspector.emissive"), comp->data.mesh_renderer.emissive);
                ImGui::DragFloat(jce_editor_i18n("inspector.normalScale"), &comp->data.mesh_renderer.normal_scale,
                                 0.01f, 0.0f, 4.0f);
                ImGui::DragFloat(jce_editor_i18n("inspector.aoStrength"), &comp->data.mesh_renderer.ao_strength,
                                 0.01f, 0.0f, 2.0f);
                {
                    const char *alpha_modes[] = {
                        jce_editor_i18n("inspector.alphaMode.opaque"),
                        jce_editor_i18n("inspector.alphaMode.mask"),
                        jce_editor_i18n("inspector.alphaMode.blend")
                    };
                    ImGui::Combo(jce_editor_i18n("inspector.alphaMode"), &comp->data.mesh_renderer.alpha_mode,
                                 alpha_modes, 3);
                }
                if (comp->data.mesh_renderer.alpha_mode == 1)
                    ImGui::DragFloat(jce_editor_i18n("inspector.alphaCutoff"),
                                     &comp->data.mesh_renderer.alpha_cutoff,
                                     0.01f, 0.0f, 1.0f);
                ImGui::Checkbox(jce_editor_i18n("inspector.doubleSided"), &comp->data.mesh_renderer.double_sided);
                ImGui::TreePop();
            }

            /* --- Texture Slots --- */
            if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.textures"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::InputText(jce_editor_i18n("inspector.texture.albedo"), comp->data.mesh_renderer.albedo_tex, 128);
                accept_asset_drop(comp->data.mesh_renderer.albedo_tex, 128);
                ImGui::InputText(jce_editor_i18n("inspector.texture.metalRough"), comp->data.mesh_renderer.mr_tex, 128);
                accept_asset_drop(comp->data.mesh_renderer.mr_tex, 128);
                ImGui::InputText(jce_editor_i18n("inspector.texture.normal"), comp->data.mesh_renderer.normal_tex, 128);
                accept_asset_drop(comp->data.mesh_renderer.normal_tex, 128);
                ImGui::InputText(jce_editor_i18n("inspector.texture.ao"), comp->data.mesh_renderer.ao_tex, 128);
                accept_asset_drop(comp->data.mesh_renderer.ao_tex, 128);
                snprintf(lbl, sizeof(lbl), "%s##tex", jce_editor_i18n("inspector.texture.emissive"));
                ImGui::InputText(lbl, comp->data.mesh_renderer.emissive_tex, 128);
                accept_asset_drop(comp->data.mesh_renderer.emissive_tex, 128);
                ImGui::TreePop();
            }
            break;

        case JCE_COMP_SPRITE_RENDERER:
            ImGui::InputText(jce_editor_i18n("spriteRenderer.sprite"), comp->data.sprite_renderer.sprite_path, 128);
            accept_asset_drop(comp->data.sprite_renderer.sprite_path, 128);
            ImGui::ColorEdit4(jce_editor_i18n("spriteRenderer.color"), comp->data.sprite_renderer.color);
            ImGui::Checkbox(jce_editor_i18n("spriteRenderer.flipX"), &comp->data.sprite_renderer.flip_x);
            ImGui::SameLine();
            ImGui::Checkbox(jce_editor_i18n("spriteRenderer.flipY"), &comp->data.sprite_renderer.flip_y);
            ImGui::DragInt(jce_editor_i18n("spriteRenderer.orderInLayer"), &comp->data.sprite_renderer.sorting_order);
            break;

        case JCE_COMP_ANIMATOR:
            ImGui::InputText(jce_editor_i18n("timeline.clip"), comp->data.animator.clip_name, 64);
            accept_asset_drop(comp->data.animator.clip_name, 64);
            ImGui::DragFloat(jce_editor_i18n("timeline.speed"), &comp->data.animator.speed,
                             0.01f, 0.0f, 10.0f);
            ImGui::Checkbox(jce_editor_i18n("timeline.loop"), &comp->data.animator.loop);
            if (ImGui::Button(comp->data.animator.playing
                              ? jce_editor_i18n("toolbar.stop")
                              : jce_editor_i18n("toolbar.play")))
                comp->data.animator.playing = !comp->data.animator.playing;
            break;

        case JCE_COMP_SKELETAL_ANIMATOR:
            ImGui::InputText(jce_editor_i18n("inspector.skeleton"), comp->data.skeletal_animator.skeleton_path, 128);
            accept_asset_drop(comp->data.skeletal_animator.skeleton_path, 128);
            snprintf(lbl, sizeof(lbl), "%s##skel", jce_editor_i18n("timeline.speed"));
            ImGui::DragFloat(lbl, &comp->data.skeletal_animator.speed,
                             0.01f, 0.0f, 10.0f);
            snprintf(lbl, sizeof(lbl), "%s##skel", jce_editor_i18n("timeline.loop"));
            ImGui::Checkbox(lbl, &comp->data.skeletal_animator.loop);
            if (comp->data.skeletal_animator.clip_count > 0) {
                ImGui::Combo(jce_editor_i18n("inspector.activeClip"), &comp->data.skeletal_animator.active_clip,
                    [](void *data, int idx) -> const char* {
                        auto *sa = (decltype(comp->data.skeletal_animator)*)data;
                        return sa->clip_names[idx]; },
                    &comp->data.skeletal_animator,
                    comp->data.skeletal_animator.clip_count);
            }
            snprintf(lbl, sizeof(lbl), "%s##skel",
                     comp->data.skeletal_animator.playing
                        ? jce_editor_i18n("toolbar.stop")
                        : jce_editor_i18n("toolbar.play"));
            if (ImGui::Button(lbl))
                comp->data.skeletal_animator.playing = !comp->data.skeletal_animator.playing;
            break;

        case JCE_COMP_RIGIDBODY:
            snprintf(lbl, sizeof(lbl), "%s##rbMass", jce_editor_i18n("rigidbody.mass"));
            ImGui::DragFloat(lbl, &comp->data.rigidbody.mass, 0.1f, 0.0f, 10000.0f);
            snprintf(lbl, sizeof(lbl), "%s##rbDrag", jce_editor_i18n("rigidbody.drag"));
            ImGui::DragFloat(lbl, &comp->data.rigidbody.drag, 0.01f, 0.0f, 100.0f);
            snprintf(lbl, sizeof(lbl), "%s##rbAngularDrag", jce_editor_i18n("rigidbody.angularDrag"));
            ImGui::DragFloat(lbl, &comp->data.rigidbody.angular_drag,
                             0.01f, 0.0f, 100.0f);
            snprintf(lbl, sizeof(lbl), "%s##rbUseGravity", jce_editor_i18n("rigidbody.useGravity"));
            ImGui::Checkbox(lbl, &comp->data.rigidbody.use_gravity);
            snprintf(lbl, sizeof(lbl), "%s##rbIsKinematic", jce_editor_i18n("rigidbody.isKinematic"));
            ImGui::Checkbox(lbl, &comp->data.rigidbody.is_kinematic);
            break;

        case JCE_COMP_BOX_COLLIDER:
            ImGui::Text("%s", jce_editor_i18n("collider.center"));
            ImGui::SameLine(80);
            draw_vec3_control("BoxCenter", comp->data.box_collider.center);
            ImGui::Text("%s", jce_editor_i18n("collider.size"));
            ImGui::SameLine(80);
            draw_vec3_control("BoxSize", comp->data.box_collider.size, 0.01f, 1.0f);
            snprintf(lbl, sizeof(lbl), "%s##box", jce_editor_i18n("collider.isTrigger"));
            ImGui::Checkbox(lbl, &comp->data.box_collider.is_trigger);
            break;

        case JCE_COMP_SPHERE_COLLIDER:
            ImGui::Text("%s", jce_editor_i18n("collider.center"));
            ImGui::SameLine(80);
            draw_vec3_control("SphereCenter", comp->data.sphere_collider.center);
            ImGui::DragFloat(jce_editor_i18n("collider.radius"), &comp->data.sphere_collider.radius,
                             0.01f, 0.001f, 1000.0f);
            snprintf(lbl, sizeof(lbl), "%s##sphere", jce_editor_i18n("collider.isTrigger"));
            ImGui::Checkbox(lbl, &comp->data.sphere_collider.is_trigger);
            break;

        case JCE_COMP_CHARACTER_CONTROLLER:
            ImGui::DragFloat(jce_editor_i18n("collider.height"), &comp->data.character_controller.height,
                             0.1f, 0.1f, 100.0f);
            snprintf(lbl, sizeof(lbl), "%s##cc", jce_editor_i18n("collider.radius"));
            ImGui::DragFloat(lbl, &comp->data.character_controller.radius,
                             0.01f, 0.01f, 50.0f);
            ImGui::DragFloat(jce_editor_i18n("inspector.stepOffset"), &comp->data.character_controller.step_offset,
                             0.01f, 0.0f, 10.0f);
            ImGui::DragFloat(jce_editor_i18n("inspector.slopeLimit"), &comp->data.character_controller.slope_limit,
                             1.0f, 0.0f, 90.0f);
            break;

        case JCE_COMP_AUDIO_SOURCE:
            ImGui::InputText(jce_editor_i18n("audioSource.clip"), comp->data.audio_source.clip_path, 128);
            accept_asset_drop(comp->data.audio_source.clip_path, 128);
            ImGui::DragFloat(jce_editor_i18n("audioSource.volume"), &comp->data.audio_source.volume,
                             0.01f, 0.0f, 1.0f);
            ImGui::DragFloat(jce_editor_i18n("audioSource.pitch"), &comp->data.audio_source.pitch,
                             0.01f, 0.01f, 3.0f);
            ImGui::DragFloat(jce_editor_i18n("inspector.spatialBlend"), &comp->data.audio_source.spatial_blend,
                             0.01f, 0.0f, 1.0f);
            snprintf(lbl, sizeof(lbl), "%s##audio", jce_editor_i18n("audioSource.loop"));
            ImGui::Checkbox(lbl, &comp->data.audio_source.loop);
            ImGui::Checkbox(jce_editor_i18n("audioSource.playOnAwake"), &comp->data.audio_source.play_on_awake);
            break;

        case JCE_COMP_SCRIPT:
            ImGui::InputText(jce_editor_i18n("inspector.script"), comp->data.script.script_path, 128);
            accept_asset_drop(comp->data.script.script_path, 128);
            break;

        default:
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.propertiesNotImplemented"));
            break;
        }
        ImGui::Spacing();
    }

    ImGui::PopID();
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
        for (int t = 0; t < JCE_COMP_TYPE_COUNT; t++) {
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
