/*
 * inspector_render.cpp
 * Render component inspector drawers: mesh renderer, sprite renderer,
 * skybox, billboard, trail, line, decal, LOD group.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_dnd.h"

static bool is_mat_json(const char *path)
{
    if (!path) return false;
    size_t len = strlen(path);
    return (len >= 9 && strcmp(path + len - 9, ".mat.json") == 0);
}

void load_material_into_renderer(JceMeshRenderer *mr)
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

/* Inverse of load_material_into_renderer: write current MeshRenderer
 * PBR/texture values back to the linked .mat.json so external ck.exe
 * (and other entities referencing the same file) see them.  Returns
 * true on success.  Skips silently if material_path is not a .mat.json. */
static bool save_renderer_to_material_file(const JceMeshRenderer *mr)
{
    if (!is_mat_json(mr->material_path)) return false;

    /* Start from defaults so any fields we don't override match the
     * engine's documented neutral PBR state. */
    JcePbrMaterial mat = jce_pbr_material_default();
    mat.base_color_factor[0] = mr->base_color[0];
    mat.base_color_factor[1] = mr->base_color[1];
    mat.base_color_factor[2] = mr->base_color[2];
    mat.base_color_factor[3] = mr->base_color[3];
    mat.metallic_factor     = mr->metallic;
    mat.roughness_factor    = mr->roughness;
    mat.emissive_factor[0]  = mr->emissive[0];
    mat.emissive_factor[1]  = mr->emissive[1];
    mat.emissive_factor[2]  = mr->emissive[2];
    mat.normal_scale        = mr->normal_scale;
    mat.ao_strength         = mr->ao_strength;
    mat.alpha_mode          = (JceAlphaMode)mr->alpha_mode;
    mat.alpha_cutoff        = mr->alpha_cutoff;
    mat.double_sided        = mr->double_sided;

    char tex_paths[5][256] = {};
    snprintf(tex_paths[0], sizeof(tex_paths[0]), "%s", mr->albedo_tex);
    snprintf(tex_paths[1], sizeof(tex_paths[1]), "%s", mr->mr_tex);
    snprintf(tex_paths[2], sizeof(tex_paths[2]), "%s", mr->normal_tex);
    snprintf(tex_paths[3], sizeof(tex_paths[3]), "%s", mr->ao_tex);
    snprintf(tex_paths[4], sizeof(tex_paths[4]), "%s", mr->emissive_tex);

    if (!jce_pbr_material_save_json(mr->material_path, &mat, tex_paths)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Failed to save material: %s", mr->material_path);
        return false;
    }
    jce_editor_console_log("Saved material: %s", mr->material_path);
    /* Push to any other MeshRenderers pointing at the same file. */
    jce_editor_inspector_reload_material(mr->material_path);
    return true;
}

static void accept_material_drop(JceMeshRenderer *mr)
{
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH)) {
            const char *path = (const char *)payload->Data;
            char rel[1024];
            const char *stored = jce_editor_path_relative_or(rel, sizeof(rel), path);
            jce_state_begin_batch_edit();
            snprintf(mr->material_path, sizeof(mr->material_path),
                     "%s", stored);
            load_material_into_renderer(mr);
            jce_state_end_batch_edit();
        }
        ImGui::EndDragDropTarget();
    }
}

void draw_comp_mesh_renderer(JceMeshRenderer *mr)
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
    jce_draw_path_input_asset("##mesh_path", mr->mesh_path, sizeof(mr->mesh_path), JCE_ASSET_KIND_MODEL);
    insp_track_edit();
    accept_mesh_drop_with_material(mr);

    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.materials"));
    ImGui::SameLine();
    jce_draw_path_input_asset("##mat_path", mr->material_path, sizeof(mr->material_path), JCE_ASSET_KIND_MATERIAL);
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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                jce_editor_i18n("meshRenderer.material.reloadHint"));
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("meshRenderer.material.save"))) {
            save_renderer_to_material_file(mr);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                jce_editor_i18n("meshRenderer.material.saveHint"));
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("meshRenderer.material.openGraph"))) {
            jce_editor_open_material_graph(mr->material_path);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s",
                jce_editor_i18n("meshRenderer.material.openGraphHint"));

        /* Assign this (graph) material to every other selected entity that
         * has a MeshRenderer, then reload so any persisted custom shader
         * takes effect.  Only meaningful with a multi-entity selection. */
        int sel_count = 0;
        const uint32_t *sel = jce_state_get_selection(&sel_count);
        if (sel_count > 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("meshRenderer.material.assignToSelection"))) {
                JceScene *scene = jce_state_get_scene();
                int assigned = 0;
                if (scene) {
                    jce_state_begin_batch_edit();
                    for (int i = 0; i < sel_count; ++i) {
                        uint32_t id = sel[i];
                        if (!id) continue;
                        JceEntity e = jce_state_to_ecs_entity(id);
                        if (!jce_scene_has_mesh_renderer(scene, e)) continue;
                        JceMeshRenderer *other = jce_scene_get_mesh_renderer(scene, e);
                        if (!other || other == mr) continue;
                        snprintf(other->material_path, sizeof(other->material_path),
                                 "%s", mr->material_path);
                        load_material_into_renderer(other);
                        ++assigned;
                    }
                    jce_state_end_batch_edit();
                }
                jce_editor_console_log(
                    "%s: %s -> %d %s",
                    jce_editor_i18n("meshRenderer.material.assignToSelection"),
                    mr->material_path, assigned,
                    jce_editor_i18n("meshRenderer.material.assignedEntities"));
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",
                    jce_editor_i18n("meshRenderer.material.assignToSelectionHint"));
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
        /* Unity-style per-renderer shadow flags. Shown in POSITIVE sense; the
         * component stores them inverted (zero-init / legacy = both ON). */
        {
            bool cast_on = !mr->shadow_cast_off;
            if (ImGui::Checkbox(jce_editor_i18n_id("inspector.castShadows",
                                                   "Cast Shadows"), &cast_on)) {
                mr->shadow_cast_off = !cast_on;
                insp_undo_bool(&mr->shadow_cast_off);
            }
            bool recv_on = !mr->shadow_receive_off;
            if (ImGui::Checkbox(jce_editor_i18n_id("inspector.receiveShadows",
                                                   "Receive Shadows"), &recv_on)) {
                mr->shadow_receive_off = !recv_on;
                insp_undo_bool(&mr->shadow_receive_off);
            }
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx(jce_editor_i18n("inspector.textures"), ImGuiTreeNodeFlags_DefaultOpen)) {
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.albedo"), mr->albedo_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(mr->albedo_tex, 128);
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.metalRough"), mr->mr_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(mr->mr_tex, 128);
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.normal"), mr->normal_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(mr->normal_tex, 128);
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.ao"), mr->ao_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(mr->ao_tex, 128);
        snprintf(lbl, sizeof(lbl), "%s###tex", jce_editor_i18n("inspector.texture.emissive"));
        jce_draw_path_input_asset(lbl, mr->emissive_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        accept_asset_drop(mr->emissive_tex, 128);
        ImGui::TreePop();
    }
}

void draw_comp_sprite_renderer(JceSpriteRendererComponent *sr)
{
    jce_draw_path_input_asset(jce_editor_i18n("spriteRenderer.sprite"), sr->sprite_path, 128, JCE_ASSET_KIND_TEXTURE);
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

void draw_comp_skybox(JceSkyboxComponent *sky)
{
    jce_draw_path_input_asset(jce_editor_i18n("skybox.hdrPath"), sky->hdr_path, 256, JCE_ASSET_KIND_TEXTURE);
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

void draw_comp_lod_group(JceLodGroupComponent *lg)
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
        jce_draw_path_input_asset(jce_editor_i18n("inspector.lod.meshOverride"), lg->level_mesh_paths[i],
                         sizeof lg->level_mesh_paths[i], JCE_ASSET_KIND_MODEL);
        insp_track_edit();
        accept_asset_drop(lg->level_mesh_paths[i], sizeof lg->level_mesh_paths[i]);
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::TextDisabled(jce_editor_i18n("inspector.lod.emptyMeshNote"));
}

void draw_comp_trail_renderer(JceTrailRendererComponent *t)
{
    insp_unwired_badge();
    if (!t) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.trail.material", "trail"), t->material_path, sizeof t->material_path, JCE_ASSET_KIND_MATERIAL);
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

void draw_comp_line_renderer(JceLineRendererComponent *l)
{
    insp_unwired_badge();
    if (!l) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.line.material", "line"), l->material_path, sizeof l->material_path, JCE_ASSET_KIND_MATERIAL);
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

void draw_comp_decal(JceDecalComponent *d)
{
    if (!d) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.decal.material", "decal"), d->material_path, sizeof d->material_path, JCE_ASSET_KIND_MATERIAL);
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

void draw_comp_billboard_renderer(JceBillboardRendererComponent *b)
{
    insp_unwired_badge();
    if (!b) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.br.texturePath", "br"), b->texture_path, sizeof b->texture_path, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    static const char *modes[] = { "Full", "Y-Axis Only" };
    int m = b->mode; if (m < 0 || m > 1) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.br.mode", "br"), &m, modes, 2)) { b->mode = m; insp_track_edit(); }
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.br.size", "br"), b->size, 0.01f, 0.0f, 1.0e4f, "%.3f"); insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.br.color", "br"), b->color)) insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.br.visible", "br"), &b->visible)) insp_undo_bool(&b->visible);
}

void draw_comp_volume(JceVolumeComponent *v)
{
    if (!v) return;

    static const char *shapes[] = { "Box", "Sphere" };
    int shape = (int)v->shape;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.vol.shape", "vol"), &shape, shapes, 2)) {
        v->shape = (JceVolumeShape)shape; insp_track_edit();
    }
    if (v->shape == JCE_VOLUME_SHAPE_BOX) {
        float ext[3] = { v->extents.x, v->extents.y, v->extents.z };
        if (ImGui::DragFloat3(jce_editor_i18n_id("inspector.vol.extents", "vol"), ext, 0.05f, 0.0f, 10000.0f, "%.3f")) {
            v->extents.x = ext[0]; v->extents.y = ext[1]; v->extents.z = ext[2]; insp_track_edit();
        }
    } else {
        if (ImGui::DragFloat(jce_editor_i18n_id("inspector.vol.radius", "vol"), &v->extents.x, 0.05f, 0.0f, 10000.0f, "%.3f"))
            insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vol.blendDistance", "vol"), &v->blend_distance, 0.05f, 0.0f, 10000.0f, "%.3f");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.vol.weight", "vol"), &v->weight, 0.01f, 0.0f, 1.0f, "%.2f");
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.vol.isGlobal", "vol"), &v->is_global))
        insp_undo_bool(&v->is_global);

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.vol.profile"));

    /* Field names and offsets for the 10 override bits */
    struct { const char *i18n; float *val; float speed; float lo; float hi; } fields[] = {
        { "inspector.vol.exposure",           &v->profile.values.exposure,           0.01f, -10.f, 10.f },
        { "inspector.vol.gamma",              &v->profile.values.gamma,              0.01f,  0.1f,  5.f },
        { "inspector.vol.bloomThreshold",     &v->profile.values.bloom_threshold,    0.01f,  0.f,  10.f },
        { "inspector.vol.bloomIntensity",     &v->profile.values.bloom_intensity,    0.01f,  0.f,  10.f },
        { "inspector.vol.fxaaSpanMax",        &v->profile.values.fxaa_span_max,      0.1f,   1.f,  16.f },
        { "inspector.vol.fxaaReduceMin",      &v->profile.values.fxaa_reduce_min,    0.001f, 0.f,   1.f },
        { "inspector.vol.fxaaReduceMul",      &v->profile.values.fxaa_reduce_mul,    0.001f, 0.f,   1.f },
        { "inspector.vol.vignetteIntensity",  &v->profile.values.vignette_intensity, 0.01f,  0.f,   2.f },
        { "inspector.vol.vignetteSmoothness", &v->profile.values.vignette_smoothness,0.01f,  0.f,   2.f },
        { "inspector.vol.chromaticStrength",  &v->profile.values.chromatic_strength, 0.01f,  0.f,   1.f },
    };
    for (int i = 0; i < 10; ++i) {
        bool en = (v->profile.enabled_mask >> i) & 1;
        ImGui::PushID(i);
        if (ImGui::Checkbox("##en", &en)) {
            if (en) v->profile.enabled_mask |=  (uint32_t)(1u << i);
            else    v->profile.enabled_mask &= ~(uint32_t)(1u << i);
            insp_track_edit();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!en);
        ImGui::DragFloat(jce_editor_i18n(fields[i].i18n), fields[i].val,
                         fields[i].speed, fields[i].lo, fields[i].hi, "%.3f");
        insp_track_edit();
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

void draw_comp_occlusion_portal(JceOcclusionPortalComponent *op)
{
    insp_unwired_badge();
    if (!op) return;
    float sz[3] = { op->size.x, op->size.y, op->size.z };
    if (ImGui::DragFloat3(jce_editor_i18n_id("inspector.op.size", "op"), sz, 0.05f, 0.0f, 10000.0f, "%.3f")) {
        op->size.x = sz[0]; op->size.y = sz[1]; op->size.z = sz[2]; insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.op.open", "op"), &op->open))
        insp_undo_bool(&op->open);
    ImGui::DragInt(jce_editor_i18n_id("inspector.op.portalId", "op"), &op->portal_id, 1.0f, 0, 65535);
    insp_track_edit();
}
