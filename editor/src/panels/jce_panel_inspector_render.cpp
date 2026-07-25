/*
 * jce_panel_inspector_render.cpp
 * Render component inspector drawers: mesh renderer, sprite renderer,
 * skybox, billboard, trail, line, decal, LOD group.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_dnd.h"

#include <jce/renderer/jce_impostor.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_mesh_lod.h>
#include "scene/jce_editor_scene_asset_cache.h"

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

/* accept_material_drop() was removed: jce_draw_path_input owns the
 * JCE_DND_ASSET_PATH target, so this second one landed on the trailing
 * browse/clear button rather than the text field.  The material-field call site
 * now reads JcePathInputOpts::dropped_raw and reloads from there. */

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
    /* The widget owns the drop target and hands back the raw absolute path;
     * the material import needs that, not the relativized mr->mesh_path. */
    char mesh_dropped[1024] = {0};
    jce_draw_path_input_asset_dnd("##mesh_path", mr->mesh_path, sizeof(mr->mesh_path),
                                  mesh_dropped, sizeof(mesh_dropped),
                                  JCE_ASSET_KIND_MODEL);
    insp_track_edit();
    if (mesh_dropped[0]) {
        jce_state_begin_batch_edit();
        apply_mesh_drop_material(mr, mesh_dropped);
        jce_state_end_batch_edit();
    }

    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s", jce_editor_i18n("meshRenderer.materials"));
    ImGui::SameLine();
    char mat_dropped[1024] = {0};
    jce_draw_path_input_asset_dnd("##mat_path", mr->material_path, sizeof(mr->material_path),
                                  mat_dropped, sizeof(mat_dropped),
                                  JCE_ASSET_KIND_MATERIAL);
    if (ImGui::IsItemDeactivatedAfterEdit() && is_mat_json(mr->material_path)) {
        jce_state_begin_batch_edit();
        load_material_into_renderer(mr);
        jce_state_end_batch_edit();
    }
    insp_track_edit();
    /* Dropping a .mat.json must load it, not just store the path.  The widget
     * has already written the relativized path into mr->material_path. */
    if (mat_dropped[0] && is_mat_json(mr->material_path)) {
        jce_state_begin_batch_edit();
        load_material_into_renderer(mr);
        jce_state_end_batch_edit();
    }
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
        /* jce_draw_path_input_asset already installs the JCE_DND_ASSET_PATH
         * drop target on the text field — no accept_asset_drop() needed. */
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.albedo"), mr->albedo_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.metalRough"), mr->mr_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.normal"), mr->normal_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        jce_draw_path_input_asset(jce_editor_i18n("inspector.texture.ao"), mr->ao_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        snprintf(lbl, sizeof(lbl), "%s###tex", jce_editor_i18n("inspector.texture.emissive"));
        jce_draw_path_input_asset(lbl, mr->emissive_tex, 128, JCE_ASSET_KIND_TEXTURE);
        insp_track_edit();
        ImGui::TreePop();
    }

    snprintf(lbl, sizeof(lbl), "%s###toon_section", jce_editor_i18n("inspector.meshRenderer.toon.section"));
    if (ImGui::TreeNodeEx(lbl, ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox(jce_editor_i18n("inspector.meshRenderer.toon.enable"), &mr->toon))
            insp_undo_bool(&mr->toon);
        ImGui::BeginDisabled(!mr->toon);
        ImGui::SliderInt(jce_editor_i18n("inspector.meshRenderer.toon.bands"), &mr->toon_bands, 2, 4);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.meshRenderer.toon.rimPower"), &mr->rim_power, 0.05f, 0.0f, 16.0f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.meshRenderer.toon.rimIntensity"), &mr->rim_intensity, 0.01f, 0.0f, 4.0f);
        insp_track_edit();
        ImGui::ColorEdit3(jce_editor_i18n("inspector.meshRenderer.toon.rimColor"), mr->rim_color);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("inspector.meshRenderer.toon.outlineWidth"), &mr->outline_width, 0.001f, 0.0f, 0.2f, "%.3f");
        insp_track_edit();
        ImGui::ColorEdit3(jce_editor_i18n("inspector.meshRenderer.toon.outlineColor"), mr->outline_color);
        insp_track_edit();
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.meshRenderer.toon.note"));
        ImGui::TreePop();
    }
}

void draw_comp_sprite_renderer(JceSpriteRendererComponent *sr)
{
    jce_draw_path_input_asset(jce_editor_i18n("spriteRenderer.sprite"), sr->sprite_path, 128, JCE_ASSET_KIND_TEXTURE);
    insp_track_edit();
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
    ImGui::DragFloat(jce_editor_i18n("skybox.rotation"), &sky->rotation, 1.0f, 0.0f, 360.0f, "%.1f deg");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("skybox.exposure"), &sky->exposure, 0.01f, 0.01f, 10.0f, "%.2f");
    insp_track_edit();
    if (sky->exposure <= 0.0f) sky->exposure = 1.0f;
    if (ImGui::Checkbox(jce_editor_i18n("skybox.useAsIbl"), &sky->use_as_ibl))
        insp_undo_bool(&sky->use_as_ibl);
}

/* Cached LOD preview for the last "Generate LODs" press (one slot; the inspector
 * shows a single LODGroup at a time).  base_tris/lod_tris hold the meshopt chain
 * the cook would bake; valid is set once a preview has been computed. */
static struct {
    char     mesh_path[256];
    bool     valid;
    uint32_t base_tris;
    uint32_t lod_tris[4];
    uint32_t lod_count;
} s_lod_preview = {};

/* Run the cook's LOD chain over the entity's loose LOD0 mesh and cache per-level
 * triangle counts for display.  Uses the SAME core the bundle cook calls, so the
 * preview matches the persisted in-asset LODs. */
static void lod_preview_generate(JceScene *scene, JceEntity e,
                                 const JceLodGroupComponent *lg)
{
    s_lod_preview.valid = false;
    s_lod_preview.lod_count = 0;

    /* LOD0 source: a LOD0 override mesh path if authored, else the entity's
     * MeshRenderer mesh (the empty-meshPath auto-bind base). */
    const char *src = NULL;
    if (lg && lg->level_mesh_paths[0][0]) src = lg->level_mesh_paths[0];
    if (!src && jce_scene_has_mesh_renderer(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr && mr->mesh_path[0]) src = mr->mesh_path;
    }
    if (!src) return;

    char host[1024];
    JceEditorCpuMeshData cpu;
    memset(&cpu, 0, sizeof(cpu));
    if (!jce_editor_resolve_asset_path(src, host, (int)sizeof(host)) ||
        !jce_editor_model_load_cpu_file(host, &cpu) ||
        !cpu.vertices || cpu.vertex_count < 3 ||
        !cpu.indices || cpu.index_count < 3) {
        jce_editor_model_free_cpu_data(&cpu);
        return;
    }

    /* Tightly packed float[3] position stream (pos@0 in JceMeshVertex). */
    std::vector<float> pos((size_t)cpu.vertex_count * 3);
    for (uint32_t v = 0; v < cpu.vertex_count; ++v) {
        pos[v * 3 + 0] = cpu.vertices[v].pos[0];
        pos[v * 3 + 1] = cpu.vertices[v].pos[1];
        pos[v * 3 + 2] = cpu.vertices[v].pos[2];
    }

    unsigned int *lvl_idx[JCE_MESH_LOD_MAX_LEVELS] = {};
    size_t        lvl_cnt[JCE_MESH_LOD_MAX_LEVELS] = {};
    size_t levels = jce_mesh_generate_lod_chain(
        pos.data(), cpu.vertex_count, 3 * sizeof(float),
        cpu.indices, cpu.index_count,
        JCE_MESH_LOD_DEFAULT_RATIOS, JCE_MESH_LOD_DEFAULT_LEVEL_COUNT,
        lvl_idx, lvl_cnt);

    s_lod_preview.base_tris = cpu.index_count / 3;
    s_lod_preview.lod_count = 0;
    for (size_t l = 0; l < levels && l < 4; ++l) {
        if (lvl_cnt[l] >= 3 && (lvl_cnt[l] % 3) == 0 && lvl_cnt[l] < cpu.index_count)
            s_lod_preview.lod_tris[s_lod_preview.lod_count++] = (uint32_t)(lvl_cnt[l] / 3);
    }
    jce_mesh_lod_chain_free(lvl_idx, levels);
    jce_editor_model_free_cpu_data(&cpu);

    snprintf(s_lod_preview.mesh_path, sizeof s_lod_preview.mesh_path, "%s", src);
    s_lod_preview.valid = true;
}

/* ── Octahedral impostor bake (roadmap P2 #10) ──────────────────────────
 * A one-shot GPU bake driven from the LODGroup inspector: render the entity's
 * mesh from grid_n x grid_n octahedral viewpoints into an atlas, read it back,
 * and write <model>.impostor.png + <model>.impostor.json next to the source so
 * the cook picks them up.  The bake is frame-driven (jce_impostor_bake_poll);
 * we poll it here each draw and, on DONE, point the LODGroup's impostor slot at
 * the produced metadata.  One bake at a time across the whole inspector. */
static struct {
    bool        active;
    int         grid_n;
    JceEntity   entity;
    char        meta_rel[256];   /* project-relative .impostor.json to assign */
    float       distance;        /* impostor distance to write on completion  */
} s_impostor_bake = { false, 8, 0, "", 200.0f };

/* Derive "<dir>/<stem>.impostor.<ext>" from a model path (host or rel). */
static void impostor_sidecar_path(const char *model_path, const char *ext,
                                  char *out, size_t cap)
{
    snprintf(out, cap, "%s", model_path ? model_path : "");
    /* strip extension */
    size_t n = strlen(out);
    for (size_t i = n; i-- > 0; ) {
        char c = out[i];
        if (c == '/' || c == '\\') break;
        if (c == '.') { out[i] = '\0'; break; }
    }
    size_t len = strlen(out);
    snprintf(out + len, cap - len, ".impostor.%s", ext);
}

static void impostor_bake_begin(JceScene *scene, JceEntity e,
                                JceLodGroupComponent *lg, int grid_n)
{
    /* Source model = LOD0 override or the entity's MeshRenderer mesh. */
    const char *src = NULL;
    if (lg->level_mesh_paths[0][0]) src = lg->level_mesh_paths[0];
    if (!src && jce_scene_has_mesh_renderer(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        if (mr && mr->mesh_path[0]) src = mr->mesh_path;
    }
    if (!src) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Impostor bake: entity has no mesh to bake");
        return;
    }

    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    JceRenderer      *r  = jce_editor_get_renderer();
    if (!sr || !r) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Impostor bake: renderer not ready");
        return;
    }
    JceModel *model = jce_scene_renderer_get_model(sr, src);
    if (!model) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Impostor bake: model '%s' not loaded — make it visible in the "
            "viewport first, then retry", src);
        return;
    }

    /* Output: <model dir>/<stem>.impostor.png + .json on the host filesystem
     * (so the cook scans them), with project-relative paths in metadata. */
    char host_dir_model[1024];
    if (!jce_editor_resolve_asset_path(src, host_dir_model, sizeof host_dir_model)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Impostor bake: cannot resolve host path for '%s'", src);
        return;
    }
    JceImpostorBakeDesc d;
    memset(&d, 0, sizeof d);
    d.model    = model;
    d.renderer = r;
    d.grid_n   = grid_n;
    d.cell_px  = 256;
    impostor_sidecar_path(host_dir_model, "png", d.atlas_path_host, sizeof d.atlas_path_host);
    impostor_sidecar_path(host_dir_model, "json", d.meta_path_host, sizeof d.meta_path_host);
    impostor_sidecar_path(src, "png", d.atlas_path_rel, sizeof d.atlas_path_rel);

    char meta_rel[256];
    impostor_sidecar_path(src, "json", meta_rel, sizeof meta_rel);

    if (!jce_impostor_bake_submit(&d)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Impostor bake: submit failed (one already running?)");
        return;
    }
    s_impostor_bake.active   = true;
    s_impostor_bake.grid_n   = grid_n;
    s_impostor_bake.entity   = e;
    s_impostor_bake.distance = lg->impostor_distance > 0.0f ? lg->impostor_distance : 200.0f;
    snprintf(s_impostor_bake.meta_rel, sizeof s_impostor_bake.meta_rel, "%s", meta_rel);
    jce_editor_console_log("Impostor bake started: %s (%dx%d views)",
                           src, grid_n, grid_n);
}

/* Drive the in-flight bake; on completion assign the LODGroup's impostor slot. */
static void impostor_bake_tick(JceScene *scene, JceLodGroupComponent *lg)
{
    if (!s_impostor_bake.active) return;
    JceImpostorBakeStatus st = jce_impostor_bake_poll();
    if (st == JCE_IMPOSTOR_BAKE_DONE) {
        s_impostor_bake.active = false;
        if (scene && lg && jce_scene_has_lod_group(scene, s_impostor_bake.entity)) {
            JceLodGroupComponent *tgt =
                jce_scene_get_lod_group(scene, s_impostor_bake.entity);
            if (tgt) {
                snprintf(tgt->impostor_meta_path, sizeof tgt->impostor_meta_path,
                         "%s", s_impostor_bake.meta_rel);
                if (tgt->impostor_distance <= 0.0f)
                    tgt->impostor_distance = s_impostor_bake.distance;
            }
        }
        jce_editor_console_log("Impostor bake complete → %s",
                               s_impostor_bake.meta_rel);
    } else if (st == JCE_IMPOSTOR_BAKE_FAILED) {
        s_impostor_bake.active = false;
        jce_editor_console_log_level(JCE_CONSOLE_ERROR, "Impostor bake failed");
    }
}

void draw_comp_lod_group(JceLodGroupComponent *lg, JceScene *scene, JceEntity e)
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
    /* Cross-fade band width (world meters) — softer LOD pop (P1 #6). */
    ImGui::DragFloat(jce_editor_i18n_id("inspector.lod.fadeWidth", "lod"), &lg->fade_width, 0.1f, 0.0f, 1000.0f, "%.1f");
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.lod.cullWhenTooFar", "lod"), &lg->cull_when_too_far))
        insp_undo_bool(&lg->cull_when_too_far);

    /* ── In-asset auto-LOD (P1 #6) ─────────────────────────────────────
     * The cook auto-generates a reduced LOD chain from any cooked mesh and
     * persists it inside the .glb (JCE_lod extension); a level with an empty
     * mesh override AUTO-BINDS that cooked chain at runtime.  "Generate LODs"
     * previews the exact chain the cook will bake (per-level triangle counts)
     * from the entity's loose LOD0 mesh. */
    ImGui::Separator();
    if (ImGui::Button(jce_editor_i18n_id("inspector.lod.generate", "lod")))
        lod_preview_generate(scene, e, lg);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.lod.generateHint"));
    if (s_lod_preview.valid && s_lod_preview.base_tris > 0) {
        ImGui::Text(jce_editor_i18n("inspector.lod.baseTrisFmt"), s_lod_preview.base_tris);
        for (uint32_t l = 0; l < s_lod_preview.lod_count; ++l) {
            float pct = 100.0f * (float)s_lod_preview.lod_tris[l] /
                        (float)s_lod_preview.base_tris;
            ImGui::Text(jce_editor_i18n("inspector.lod.lodTrisFmt"), l + 1,
                        s_lod_preview.lod_tris[l], pct);
        }
        if (s_lod_preview.lod_count == 0)
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.lod.noReduce"));
    }

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
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::TextDisabled(jce_editor_i18n("inspector.lod.emptyMeshNote"));

    /* ── Octahedral impostor terminal LOD (roadmap P2 #10) ─────────────
     * Beyond the last mesh LOD, far props render as a single camera-facing card
     * sampling a pre-baked octahedral atlas (a whole forest = a few instanced
     * quads).  "Bake Impostor" GPU-renders the mesh from grid_n x grid_n angles,
     * writes <model>.impostor.png + .json, and points this slot at them. */
    ImGui::Separator();
    ImGui::TextColored(JCE_COLOR_INSP_LABEL, "%s",
                       jce_editor_i18n_or("inspector.lod.impostorHeader",
                                          "Impostor (terminal LOD)"));

    impostor_bake_tick(scene, lg);

    ImGui::DragFloat(jce_editor_i18n_id("inspector.lod.impostorDistance", "lod.impostorDistance"),
                     &lg->impostor_distance, 1.0f, 0.0f, 100000.0f, "%.1f m");
    insp_track_edit();

    ImGui::SliderInt(jce_editor_i18n_id("inspector.lod.impostorGrid", "lod.impostorGrid"),
                     &s_impostor_bake.grid_n, 4, 10);

    if (s_impostor_bake.active) {
        ImGui::ProgressBar(jce_impostor_bake_progress(), ImVec2(-1, 0), jce_editor_i18n("inspector.lod.baking"));
    } else {
        if (ImGui::Button(jce_editor_i18n_id("inspector.lod.impostorBake", "lod.impostorBake"))) {
            int gn = s_impostor_bake.grid_n;
            if (gn < 4) gn = 8;
            impostor_bake_begin(scene, e, lg, gn);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("inspector.lod.impostorBakeTip"));
    }

    /* Bound impostor metadata path + atlas preview. */
    jce_draw_path_input_asset("Impostor Meta###lod.impostorMeta",
                     lg->impostor_meta_path, sizeof lg->impostor_meta_path,
                     JCE_ASSET_KIND_MODEL);
    insp_track_edit();

    if (lg->impostor_meta_path[0]) {
        JceImpostorMeta meta;
        char host_meta[1024];
        if (jce_editor_resolve_asset_path(lg->impostor_meta_path, host_meta,
                                          sizeof host_meta) &&
            jce_impostor_meta_read(host_meta, &meta)) {
            ImGui::Text(jce_editor_i18n("inspector.lod.atlasFmt"), meta.grid_n, meta.grid_n,
                        meta.radius);
            /* Resolve the atlas PNG through the editor asset cache (same path the
             * scene renderer uses) and preview it. */
            JceTexture at = jce_editor_scene_asset_cache_get_texture(
                                meta.atlas_path, NULL);
            if (jce_texture_valid(at)) {
                ImGui::Image((ImTextureID)(uintptr_t)((uint32_t)at.idx + 1u),
                             ImVec2(128, 128));
            }
        } else {
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.lod.impostorMetaMissing"));
        }
    }
}

void draw_comp_trail_renderer(JceTrailRendererComponent *t)
{
    /* Wired (large-world #E): sr_draw_trail_renderer draws the captured point
     * buffer as a ribbon; the runtime grows it (rt_update_trails). */
    if (!t) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.trail.material", "trail"), t->material_path, sizeof t->material_path, JCE_ASSET_KIND_MATERIAL);
    insp_track_edit();
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
    /* Wired (large-world #E): sr_draw_line_renderer draws the polyline in the
     * color pass (reuses the color program + PT_LINES). No longer unwired. */
    if (!l) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.line.material", "line"), l->material_path, sizeof l->material_path, JCE_ASSET_KIND_MATERIAL);
    insp_track_edit();
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
    /* Wired (large-world #E): the scene renderer draws a camera-facing textured
     * quad via the sprite batch (FULL / Y-axis). No longer unwired. */
    if (!b) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.br.texturePath", "br"), b->texture_path, sizeof b->texture_path, JCE_ASSET_KIND_TEXTURE); insp_track_edit();
    const char *modes[] = { jce_editor_i18n("inspector.br.mode.full"), jce_editor_i18n("inspector.br.mode.yAxisOnly") };
    int m = b->mode; if (m < 0 || m > 1) m = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.br.mode", "br"), &m, modes, 2)) { b->mode = m; insp_track_edit(); }
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.br.size", "br"), b->size, 0.01f, 0.0f, 1.0e4f, "%.3f"); insp_track_edit();
    if (ImGui::ColorEdit4(jce_editor_i18n_id("inspector.br.color", "br"), b->color)) insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.br.visible", "br"), &b->visible)) insp_undo_bool(&b->visible);
}

void draw_comp_volume(JceVolumeComponent *v)
{
    if (!v) return;

    const char *shapes[] = { jce_editor_i18n("inspector.vol.shape.box"), jce_editor_i18n("inspector.vol.shape.sphere") };
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
