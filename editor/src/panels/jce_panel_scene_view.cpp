/*
 * jce_panel_scene_view.cpp  Scene View panel — toolbar, camera, context menu,
 *                           shortcuts, overlays, main content.
 *
 * Helper icons & entity creation are in jce_scene_view_helpers.cpp.
 * Axis indicator & view cube are in jce_scene_view_cube.cpp.
 * Gizmo interaction & selection box are in jce_scene_view_gizmo.cpp.
 */

#include "jce_scene_view_internal.h"
#include "core/jce_editor_i18n.h"
#include "scene/jce_editor_scene_asset_cache.h"
#include "core/jce_hotkeys.h"
#include "core/jce_editor_config.h"
#include "ui/jce_editor_panels.h"
#include <jce/os/core/jce_filesystem.h>

extern "C" {
#include <jce/renderer/jce_pbr_material.h>
#include <jce/middleware/scene/jce_terrain.h>

bool jce_terrain_panel_brush_armed(void);
struct JceTerrain *jce_terrain_panel_get_terrain(void);
void jce_terrain_panel_apply_brush_world(float wx, float wz, float dt);
}

#include <ctype.h>

/* ── Forward declarations ─────────────────────────────────────────── */
static void handle_scene_view_asset_drop(ImVec2 screen_pos, ImVec2 avail);
static void scene_view_frame_entities(bool all);

/* ── Toolbar ─────────────────────────────────────────────────────── */

static void draw_scene_view_toolbar(void)
{
    JceGizmoMode gm = jce_state_get_gizmo_mode();
    if (ImGui::RadioButton("T", gm == JCE_GIZMO_TRANSLATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("sceneView.tooltip.translate"));
    ImGui::SameLine();
    if (ImGui::RadioButton("R", gm == JCE_GIZMO_ROTATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("sceneView.tooltip.rotate"));
    ImGui::SameLine();
    if (ImGui::RadioButton("S", gm == JCE_GIZMO_SCALE))
        jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("sceneView.tooltip.scale"));

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    JceGizmoSpace gs = jce_state_get_gizmo_space();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###local", jce_editor_i18n("toolbar.local"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_LOCAL))
            jce_state_set_gizmo_space(JCE_GIZMO_LOCAL);
    }
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###world", jce_editor_i18n("toolbar.global"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_WORLD))
            jce_state_set_gizmo_space(JCE_GIZMO_WORLD);
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    bool is_2d = jce_state_get_2d_mode();
    if (ImGui::RadioButton(jce_editor_i18n("sceneView.mode2d"), is_2d))
        jce_state_set_2d_mode(true);
    ImGui::SameLine();
    if (ImGui::RadioButton(jce_editor_i18n("sceneView.mode3d"), !is_2d))
        jce_state_set_2d_mode(false);

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    if (ImGui::Button(jce_editor_i18n("toolbar.frameSelected"))) {
        scene_view_frame_entities(false);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s (F)", jce_editor_i18n("toolbar.frameSelected"));

    ImGui::SameLine();

    if (ImGui::Button(jce_editor_i18n("menu.view"))) {
        ImGui::OpenPopup("##SceneViewMenu");
    }
    if (ImGui::BeginPopup("##SceneViewMenu")) {
        JceSceneViewMode vm = jce_state_get_view_mode();

        if (ImGui::BeginMenu(jce_editor_i18n("sceneView.renderMode"))) {
            if (ImGui::BeginMenu(jce_editor_i18n("scene.wireframe"))) {
                if (ImGui::MenuItem(jce_editor_i18n("scene.wireframe"), NULL, vm == JCE_VIEW_WIREFRAME))
                    jce_state_set_view_mode(JCE_VIEW_WIREFRAME);
                if (ImGui::MenuItem(jce_editor_i18n("scene.wireframeTextured"), NULL, vm == JCE_VIEW_WIREFRAME_TEXTURED))
                    jce_state_set_view_mode(JCE_VIEW_WIREFRAME_TEXTURED);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.shaded"), NULL, vm == JCE_VIEW_SHADED))
                jce_state_set_view_mode(JCE_VIEW_SHADED);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.textured"), NULL, vm == JCE_VIEW_TEXTURED))
                jce_state_set_view_mode(JCE_VIEW_TEXTURED);
            ImGui::EndMenu();
        }

        bool grid = jce_state_get_show_grid();
        if (ImGui::MenuItem(jce_editor_i18n("scene.grid"), NULL, grid))
            jce_state_set_show_grid(!grid);

        bool phys = jce_state_get_show_physics_debug();
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.physicsDebug"), "", phys))
            jce_state_set_show_physics_debug(!phys);

        if (ImGui::BeginMenu(jce_editor_i18n("sceneView.menu.showFlags"))) {
            uint32_t f = jce_state_get_show_flags();
            struct { const char *i18n_key; uint32_t bit; } items[] = {
                { "sceneView.flag.gizmos",        JCE_SHOW_FLAG_GIZMOS         },
                { "sceneView.flag.lightIcons",    JCE_SHOW_FLAG_LIGHT_ICONS    },
                { "sceneView.flag.cameraIcons",   JCE_SHOW_FLAG_CAMERA_ICONS   },
                { "sceneView.flag.colliders",     JCE_SHOW_FLAG_COLLIDERS      },
                { "sceneView.flag.skybox",        JCE_SHOW_FLAG_SKYBOX         },
                { "sceneView.flag.boundingBoxes", JCE_SHOW_FLAG_BOUNDING_BOXES },
                { "sceneView.flag.worldAxis",     JCE_SHOW_FLAG_WORLD_AXIS     },
                { "sceneView.flag.statsOverlay",  JCE_SHOW_FLAG_STATS_OVERLAY  },
                { "sceneView.flag.navMesh",       JCE_SHOW_FLAG_NAVMESH        },
            };
            for (auto &it : items) {
                bool on = (f & it.bit) != 0;
                if (ImGui::MenuItem(jce_editor_i18n(it.i18n_key), NULL, on))
                    jce_state_set_show_flag((JceShowFlag)it.bit, !on);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.menu.allOn")))   jce_state_set_show_flags(0xFFFFFFFFu);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.menu.allOff")))  jce_state_set_show_flags(0);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.menu.defaults"))) jce_state_set_show_flags(
                JCE_SHOW_FLAG_GIZMOS | JCE_SHOW_FLAG_LIGHT_ICONS
              | JCE_SHOW_FLAG_CAMERA_ICONS | JCE_SHOW_FLAG_SKYBOX
              | JCE_SHOW_FLAG_WORLD_AXIS);
            ImGui::EndMenu();
        }

        /* (livePreview menu item removed — the underlying
           jce_state_live_preview flag was a no-op: nothing ever read it
           to change behavior. If/when real-time preview lands, restore
           this entry and wire the flag into the render loop.) */

        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("sceneView.resetCamera")))  {
            jce_editor_scene_camera_reset();
        }
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.topView")))      { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_TOP); }
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.frontView")))    { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_FRONT); }
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.sideView")))     { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_RIGHT); }

        ImGui::EndPopup();
    }

    ImGui::Separator();
}

/* ── Viewport setup ──────────────────────────────────────────────── */

static bool setup_scene_viewport(SceneViewCtx *ctx)
{
    ImVec2 avail_raw = ImGui::GetContentRegionAvail();
    if (avail_raw.x <= 0 || avail_raw.y <= 0) {
        clear_stale_gizmo_interaction_state();
        return false;
    }

    /* Use the real avail size as the render target. Original FBO churn
     * mitigation (32-px quantization) caused a visible aspect mismatch
     * because the renderer fills the quantized texture but ImGui
     * displays the avail rect — switching back to real size now that
     * the offscreen-target retire pool handles the handle-recycle race
     * that originally caused crashes during drag-resize. */
    uint32_t avail_w = (uint32_t)fmaxf(1.0f, floorf(avail_raw.x));
    uint32_t avail_h = (uint32_t)fmaxf(1.0f, floorf(avail_raw.y));
    uint32_t vp_w = avail_w;
    uint32_t vp_h = avail_h;
    if (vp_w < 16u) vp_w = 16u;
    if (vp_h < 16u) vp_h = 16u;
    ImVec2 avail((float)avail_w, (float)avail_h);

    float uv_u1 = 1.0f;
    float uv_v1 = 1.0f;

    ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(screen_pos,
                      ImVec2(screen_pos.x + avail.x, screen_pos.y + avail.y),
                      IM_COL32(30, 30, 40, 255));

    jce_editor_scene_render_frame(vp_w, vp_h);

    uint16_t tex_idx = jce_editor_scene_render_get_texture();
    if (tex_idx != UINT16_MAX) {
        const bool origin_bl = jce_renderer_origin_bottom_left();
        if (origin_bl) {
            ImGui::Image((ImTextureID)(uintptr_t)tex_idx, avail,
                         ImVec2(0.0f, uv_v1), ImVec2(uv_u1, 0.0f));
        } else {
            ImGui::Image((ImTextureID)(uintptr_t)tex_idx, avail,
                         ImVec2(0.0f, 0.0f), ImVec2(uv_u1, uv_v1));
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(screen_pos.x + 8, screen_pos.y + 8));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.6f),
            "%s  %.0f x %.0f", jce_editor_i18n("Scene"), avail.x, avail.y);
    }

    ImGui::SetCursorScreenPos(screen_pos);
    ImGui::InvisibleButton("##SceneViewInput", avail);
    handle_scene_view_asset_drop(screen_pos, avail);  /* drop target must follow the button immediately */
    bool viewport_hovered = ImGui::IsItemHovered();
    (void)ImGui::IsItemActive();

    ctx->avail            = avail;
    ctx->screen_pos       = screen_pos;
    ctx->dl               = dl;
    ctx->viewport_hovered = viewport_hovered;
    return true;
}

/* ── Asset drag-and-drop into the viewport ───────────────────────── */

/* Lowercase a path's extension into a small buffer (incl. leading dot). */
static void copy_ext_lower(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path) return;
    const char *ext = strrchr(path, '.');
    if (!ext) return;
    size_t i = 0;
    for (const char *p = ext; *p && i + 1 < out_size; ++p, ++i)
        out[i] = (char)tolower((unsigned char)*p);
    out[i] = '\0';
}

/* Returns true if the extension matches a supported 3-D mesh format. */
static bool is_mesh_asset(const char *path)
{
    char ext[16];
    copy_ext_lower(path, ext, sizeof(ext));
    if (ext[0] == '\0') return false;
    static const char *const mesh_exts[] = {
        ".fbx", ".glb", ".gltf", ".obj", ".mesh", ".dae",
        ".stl", ".ply", ".usd", ".usdc", ".usdz",
        ".3ds", ".blend",
        NULL
    };
    for (int i = 0; mesh_exts[i]; i++) {
        if (strcmp(ext, mesh_exts[i]) == 0)
            return true;
    }
    return false;
}

/* Returns true if the extension matches a supported texture/material format. */
static bool is_texture_or_material_asset(const char *path)
{
    if (!path) return false;
    /* Check for compound .mat.json extension (case-insensitive). */
    size_t plen = strlen(path);
    if (plen >= 9) {
        const char *tail = path + plen - 9;
        bool mat_json = true;
        const char *expect = ".mat.json";
        for (int i = 0; i < 9 && mat_json; i++) {
            if ((char)tolower((unsigned char)tail[i]) != expect[i])
                mat_json = false;
        }
        if (mat_json) return true;
    }
    char ext[16];
    copy_ext_lower(path, ext, sizeof(ext));
    if (ext[0] == '\0') return false;
    static const char *const tex_exts[] = {
        ".png", ".jpg", ".jpeg", ".jfif",
        ".tga", ".bmp", ".dds", ".ktx", ".ktx2",
        ".tif", ".tiff", ".gif", ".webp", ".psd",
        ".hdr", ".exr",
        NULL
    };
    for (int i = 0; tex_exts[i]; i++) {
        if (strcmp(ext, tex_exts[i]) == 0)
            return true;
    }
    return false;
}

/* Returns true if the file is an HDR environment map. */
static bool is_hdr_asset(const char *path)
{
    char ext[16];
    copy_ext_lower(path, ext, sizeof(ext));
    return (strcmp(ext, ".hdr") == 0 || strcmp(ext, ".exr") == 0);
}

static JceMeshRenderer *find_mesh_renderer_component(uint32_t entity_id)
{
    if (entity_id == 0 || !jce_state_entity_exists(entity_id)) return NULL;
    JceScene *scene = jce_state_get_scene();
    if (!scene) return NULL;
    return jce_scene_get_mesh_renderer(scene, (JceEntity)entity_id);
}

static bool entity_accepts_mesh_material_drop(uint32_t entity_id)
{
    return find_mesh_renderer_component(entity_id) != NULL;
}

static int detect_texture_drop_slot(const char *path)
{
    const char *name = path ? path : "";
    const char *sep = strrchr(name, '/');
    const char *sep2 = strrchr(name, '\\');
    if (sep2 > sep) sep = sep2;
    if (sep) name = sep + 1;

    char lowered[256];
    snprintf(lowered, sizeof(lowered), "%s", name);
    for (int index = 0; lowered[index] != '\0'; index++)
        lowered[index] = (char)tolower((unsigned char)lowered[index]);

    if (strstr(lowered, "normal") || strstr(lowered, "_nor")
        || strstr(lowered, "_nrm") || strstr(lowered, "nrm"))
        return 2;

    if (strstr(lowered, "occlusion") || strstr(lowered, "ambientocclusion")
        || strstr(lowered, "ambient_occlusion") || strstr(lowered, "_ao")
        || strstr(lowered, "ao."))
        return 3;

    if (strstr(lowered, "emissive") || strstr(lowered, "emission")
        || strstr(lowered, "emit"))
        return 4;

    if (strstr(lowered, "metallicroughness") || strstr(lowered, "metalrough")
        || strstr(lowered, "metal_rough") || strstr(lowered, "roughness")
        || strstr(lowered, "metallic") || strstr(lowered, "_mr")
        || strstr(lowered, "orm") || strstr(lowered, "rma"))
        return 1;

    return 0;
}

static void assign_texture_drop_to_mesh_renderer(JceMeshRenderer *mesh_renderer_comp,
                                                 int slot,
                                                 const char *asset_path)
{
    if (!mesh_renderer_comp) return;

    /* Always store project-relative so component paths survive
     * cwd / project moves and don't leak the user's home dir. */
    char rel[1024];
    jce_editor_path_to_relative(rel, sizeof(rel), asset_path);
    const char *store_path = rel[0] ? rel : asset_path;

    auto &mr = *mesh_renderer_comp;
    switch (slot) {
    case 1:
        snprintf(mr.mr_tex, sizeof(mr.mr_tex), "%s", store_path);
        break;
    case 2:
        snprintf(mr.normal_tex, sizeof(mr.normal_tex), "%s", store_path);
        break;
    case 3:
        snprintf(mr.ao_tex, sizeof(mr.ao_tex), "%s", store_path);
        break;
    case 4:
        snprintf(mr.emissive_tex, sizeof(mr.emissive_tex), "%s", store_path);
        break;
    case 0:
    default:
        snprintf(mr.albedo_tex, sizeof(mr.albedo_tex), "%s", store_path);
        break;
    }
}

static bool apply_material_asset_to_mesh_renderer(JceMeshRenderer *mesh_renderer_comp,
                                                  const char *asset_path)
{
    if (!mesh_renderer_comp) return false;

    JcePbrMaterial material = {};
    char tex_paths[5][256] = {};
    /* Load uses the absolute host path; the value we *store* into the
     * component must be project-relative so it round-trips through save. */
    if (!jce_pbr_material_load_json(asset_path, &material, tex_paths))
        return false;

    char rel[1024];
    jce_editor_path_to_relative(rel, sizeof(rel), asset_path);
    const char *store_path = rel[0] ? rel : asset_path;

    auto &mr = *mesh_renderer_comp;
    snprintf(mr.material_path, sizeof(mr.material_path), "%s", store_path);
    mr.albedo_tex[0] = '\0';
    mr.mr_tex[0] = '\0';
    mr.normal_tex[0] = '\0';
    mr.ao_tex[0] = '\0';
    mr.emissive_tex[0] = '\0';

    if (tex_paths[0][0])
        snprintf(mr.albedo_tex, sizeof(mr.albedo_tex), "%s", tex_paths[0]);
    if (tex_paths[1][0])
        snprintf(mr.mr_tex, sizeof(mr.mr_tex), "%s", tex_paths[1]);
    if (tex_paths[2][0])
        snprintf(mr.normal_tex, sizeof(mr.normal_tex), "%s", tex_paths[2]);
    if (tex_paths[3][0])
        snprintf(mr.ao_tex, sizeof(mr.ao_tex), "%s", tex_paths[3]);
    if (tex_paths[4][0])
        snprintf(mr.emissive_tex, sizeof(mr.emissive_tex), "%s", tex_paths[4]);

    mr.base_color[0] = material.base_color_factor[0];
    mr.base_color[1] = material.base_color_factor[1];
    mr.base_color[2] = material.base_color_factor[2];
    mr.base_color[3] = material.base_color_factor[3];
    mr.metallic = material.metallic_factor;
    mr.roughness = material.roughness_factor;
    mr.emissive[0] = material.emissive_factor[0];
    mr.emissive[1] = material.emissive_factor[1];
    mr.emissive[2] = material.emissive_factor[2];
    mr.normal_scale = material.normal_scale;
    mr.ao_strength = material.ao_strength;
    mr.alpha_mode = (int)material.alpha_mode;
    mr.alpha_cutoff = material.alpha_cutoff;
    mr.double_sided = material.double_sided;
    return true;
}

static void apply_extracted_material_to_mesh_renderer(
    JceMeshRenderer *mesh_renderer_comp,
    const JceEditorMaterialInfo *material)
{
    if (!mesh_renderer_comp || !material) return;

    auto &mr = *mesh_renderer_comp;
    if (material->albedo_tex[0])
        snprintf(mr.albedo_tex, sizeof(mr.albedo_tex), "%s", material->albedo_tex);
    if (material->mr_tex[0])
        snprintf(mr.mr_tex, sizeof(mr.mr_tex), "%s", material->mr_tex);
    if (material->normal_tex[0])
        snprintf(mr.normal_tex, sizeof(mr.normal_tex), "%s", material->normal_tex);
    if (material->ao_tex[0])
        snprintf(mr.ao_tex, sizeof(mr.ao_tex), "%s", material->ao_tex);
    if (material->emissive_tex[0])
        snprintf(mr.emissive_tex, sizeof(mr.emissive_tex), "%s", material->emissive_tex);

    mr.base_color[0] = material->base_color[0];
    mr.base_color[1] = material->base_color[1];
    mr.base_color[2] = material->base_color[2];
    mr.base_color[3] = material->base_color[3];
    mr.metallic       = material->metallic;
    mr.roughness      = material->roughness;
    mr.emissive[0]    = material->emissive[0];
    mr.emissive[1]    = material->emissive[1];
    mr.emissive[2]    = material->emissive[2];
    mr.normal_scale   = material->normal_scale;
    mr.ao_strength    = material->ao_strength;
    mr.alpha_mode     = material->alpha_mode;
    mr.alpha_cutoff   = material->alpha_cutoff;
    mr.double_sided   = material->double_sided;
}

static void flush_async_drop_material_extracts(void)
{
    JceEditorMaterialExtractResult result = {};
    bool has_updates = false;
    bool transient_edit_open = false;

    while (jce_editor_scene_asset_cache_take_material_result(&result)) {
        if (!result.success) {
            jce_editor_console_log_level(
                JCE_CONSOLE_WARNING,
                "Dropped mesh material extraction failed: %s",
                result.mesh_path);
            continue;
        }

        JceMeshRenderer *mesh_renderer_comp =
            find_mesh_renderer_component(result.entity_id);
        if (!mesh_renderer_comp)
            continue;

        if (strcmp(mesh_renderer_comp->mesh_path, result.mesh_path) != 0)
            continue;

        if (!transient_edit_open) {
            jce_state_begin_transient_edit();
            transient_edit_open = true;
        }
        apply_extracted_material_to_mesh_renderer(mesh_renderer_comp,
                                                  &result.material);
        has_updates = true;
    }

    if (transient_edit_open)
        jce_state_end_transient_edit();

    if (has_updates)
        jce_editor_inspector_request_sync();
}

/* Ray-cast pick: find the nearest entity under the current mouse position.
 * Returns entity ID (0 = none). */
static uint32_t pick_entity_at_mouse(ImVec2 screen_pos, ImVec2 avail)
{
    float view_mat[16], proj_mat[16], eye[3];
    if (!jce_editor_scene_get_camera_matrices(view_mat, proj_mat, eye,
                                              avail.x, avail.y))
        return 0;

    JceGizmoCamera cam;
    memcpy(cam.view, view_mat, sizeof(float) * 16);
    memcpy(cam.proj, proj_mat, sizeof(float) * 16);
    memcpy(cam.eye,  eye,      sizeof(float) * 3);
    cam.viewport_size[0]   = avail.x;
    cam.viewport_size[1]   = avail.y;
    cam.viewport_origin[0] = screen_pos.x;
    cam.viewport_origin[1] = screen_pos.y;

    ImVec2 mouse = ImGui::GetMousePos();
    float ray_o[3], ray_d[3];
    gm_screen_to_ray(&cam, mouse.x, mouse.y, ray_o, ray_d);

    uint32_t best_id = 0;
    float    best_t  = 1e30f;

    JceScene *scene = jce_state_get_scene();
    int total = jce_state_get_entity_count();
    for (int pi = 0; pi < total; pi++) {
        uint32_t pid = jce_state_get_entity_id_by_index(pi);
        if (pid == 0 || !jce_state_entity_exists(pid)) continue;
        if (!jce_state_entity_enabled(pid)) continue;

        JceTransform *t = scene ? jce_scene_get_transform(scene, (JceEntity)pid) : NULL;
        if (!t) continue;
        float pos[3] = { t->position.x, t->position.y, t->position.z };
        float scl[3] = { t->scale.x,    t->scale.y,    t->scale.z    };

        float hx = fabsf(scl[0]) * 0.5f;
        float hy = fabsf(scl[1]) * 0.5f;
        float hz = fabsf(scl[2]) * 0.5f;
        if (hx < 0.1f) hx = 0.1f;
        if (hy < 0.1f) hy = 0.1f;
        if (hz < 0.1f) hz = 0.1f;

        float bmin[3] = { pos[0]-hx, pos[1]-hy, pos[2]-hz };
        float bmax[3] = { pos[0]+hx, pos[1]+hy, pos[2]+hz };

        float t_hit;
        jce_vec3 ro    = jce_v3(ray_o[0], ray_o[1], ray_o[2]);
        jce_vec3 rd    = jce_v3(ray_d[0], ray_d[1], ray_d[2]);
        jce_vec3 bminv = jce_v3(bmin[0],  bmin[1],  bmin[2]);
        jce_vec3 bmaxv = jce_v3(bmax[0],  bmax[1],  bmax[2]);
        if (jce_ray_aabb_intersect(ro, rd, bminv, bmaxv, &t_hit) && t_hit >= 0.0f) {
            if (t_hit < best_t) {
                best_t  = t_hit;
                best_id = pid;
            }
        }
    }
    return best_id;
}

/* Compute the world-space hit position on the Y=0 ground plane from
 * the current mouse position, using camera unprojection. */
static bool compute_ground_hit(ImVec2 screen_pos, ImVec2 avail, float out_pos[3])
{
    float view_mat[16], proj_mat[16], eye[3];
    if (!jce_editor_scene_get_camera_matrices(view_mat, proj_mat, eye,
                                              avail.x, avail.y))
        return false;

    JceGizmoCamera cam;
    memcpy(cam.view, view_mat, sizeof(float) * 16);
    memcpy(cam.proj, proj_mat, sizeof(float) * 16);
    memcpy(cam.eye,  eye,      sizeof(float) * 3);
    cam.viewport_size[0]   = avail.x;
    cam.viewport_size[1]   = avail.y;
    cam.viewport_origin[0] = screen_pos.x;
    cam.viewport_origin[1] = screen_pos.y;

    ImVec2 mouse = ImGui::GetMousePos();
    float ray_o[3], ray_d[3];
    gm_screen_to_ray(&cam, mouse.x, mouse.y, ray_o, ray_d);

    /* Intersect with Y=0 ground plane. */
    if (fabsf(ray_d[1]) < 1e-6f) return false;
    float t = -ray_o[1] / ray_d[1];
    if (t < 0.0f) return false;  /* behind camera */
    out_pos[0] = ray_o[0] + ray_d[0] * t;
    out_pos[1] = 0.0f;
    out_pos[2] = ray_o[2] + ray_d[2] * t;
    return true;
}

/* Frame the selection (or all entities) in the scene view by computing
 * an AABB from JceTransform positions/scales and asking the editor scene
 * camera to fit it. Shared by the F / Shift+F hotkeys and the toolbar. */
static void scene_view_frame_entities(bool all)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    float bmin[3] = { 1e30f,  1e30f,  1e30f };
    float bmax[3] = {-1e30f, -1e30f, -1e30f };
    int counted = 0;

    auto accumulate = [&](uint32_t id) {
        JceTransform *t = jce_scene_get_transform(scene, (JceEntity)id);
        if (!t) return;
        float hx = fabsf(t->scale.x) * 0.5f;
        float hy = fabsf(t->scale.y) * 0.5f;
        float hz = fabsf(t->scale.z) * 0.5f;
        if (hx < 0.1f) hx = 0.1f;
        if (hy < 0.1f) hy = 0.1f;
        if (hz < 0.1f) hz = 0.1f;
        float lo[3] = { t->position.x - hx, t->position.y - hy, t->position.z - hz };
        float hi[3] = { t->position.x + hx, t->position.y + hy, t->position.z + hz };
        for (int k = 0; k < 3; k++) {
            if (lo[k] < bmin[k]) bmin[k] = lo[k];
            if (hi[k] > bmax[k]) bmax[k] = hi[k];
        }
        counted++;
    };

    if (all) {
        int total = jce_state_get_entity_count();
        for (int i = 0; i < total; i++) {
            uint32_t id = jce_state_get_entity_id_by_index(i);
            if (id != 0 && jce_state_entity_exists(id))
                accumulate(id);
        }
    } else {
        int sel_n = 0;
        const uint32_t *sel = jce_state_get_selection(&sel_n);
        if (sel_n == 0) {
            uint32_t f_ent = jce_state_get_focused();
            if (f_ent != 0) accumulate(f_ent);
        } else {
            for (int i = 0; i < sel_n; i++)
                if (sel[i] != 0 && jce_state_entity_exists(sel[i]))
                    accumulate(sel[i]);
        }
    }

    if (counted > 0)
        jce_editor_scene_camera_focus_aabb(bmin, bmax);
}

/* Called immediately after InvisibleButton so the drag-drop target
 * applies to the full viewport area. */
static void handle_scene_view_asset_drop(ImVec2 screen_pos, ImVec2 avail){
    if (!ImGui::BeginDragDropTarget()) {
        jce_editor_scene_clear_ghost();
        jce_editor_scene_clear_hover_entity();
        return;
    }

    /* ── Ghost / hover preview while hovering ───────────────────── */
    if (const ImGuiPayload *peek =
            ImGui::AcceptDragDropPayload("JCE_ASSET_PATH",
                                         ImGuiDragDropFlags_AcceptPeekOnly)) {
        const char *asset_path = (const char *)peek->Data;
        /* HDR must be checked BEFORE the generic texture branch because
         * is_texture_or_material_asset() also matches *.hdr — without
         * this ordering an HDR drop on empty viewport would fall into
         * the texture branch and silently no-op. */
        if (is_hdr_asset(asset_path)) {
            /* HDR environment map: no ghost, just clear state. */
            jce_editor_scene_clear_ghost();
            jce_editor_scene_clear_hover_entity();
        } else if (is_mesh_asset(asset_path)) {
            /* Mesh drag: ghost preview on ground plane, or highlight
             * entity if the cursor is over one (mesh-on-entity = replace). */
            uint32_t hit_id = pick_entity_at_mouse(screen_pos, avail);
            if (entity_accepts_mesh_material_drop(hit_id)) {
                jce_editor_scene_clear_ghost();
                jce_editor_scene_set_hover_entity(hit_id);
            } else {
                jce_editor_scene_clear_hover_entity();
                float hit[3];
                if (compute_ground_hit(screen_pos, avail, hit))
                    jce_editor_scene_set_ghost(asset_path, hit[0], hit[1], hit[2]);
            }
        } else if (is_texture_or_material_asset(asset_path)) {
            /* Texture / material drag: highlight entity under cursor. */
            jce_editor_scene_clear_ghost();
            uint32_t hit_id = pick_entity_at_mouse(screen_pos, avail);
            if (!entity_accepts_mesh_material_drop(hit_id))
                hit_id = 0;
            jce_editor_scene_set_hover_entity(hit_id);
        } else {
            jce_editor_scene_clear_ghost();
            jce_editor_scene_clear_hover_entity();
        }
    }

    /* ── Actual drop ────────────────────────────────────────────── */
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
        const char *asset_path = (const char *)payload->Data;

        jce_editor_scene_clear_ghost();
        jce_editor_scene_clear_hover_entity();

        /* Surface a warning early if the file does not exist on disk —
         * this is the most common silent-failure cause when dragging from
         * a stale asset browser cache or a path that contains characters
         * the host filesystem rejects. */
        if (asset_path && asset_path[0] != '\0'
            && !jce_fs_host_exists_file(asset_path)) {
            jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                "Dropped asset path does not exist on disk: '%s' "
                "(check the asset browser, working directory, or that "
                "the file was not moved/renamed).",
                asset_path);
        }

        /* ── HDR dropped: set as skybox (checked first so that HDR
         *    files do not get caught by the generic texture branch
         *    below — is_texture_or_material_asset() also matches .hdr) */
        if (is_hdr_asset(asset_path)) {
            if (!jce_fs_host_exists_file(asset_path)) {
                jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                    "HDR file not found, cannot apply skybox: %s", asset_path);
            } else {
            JceScene *scene = jce_state_get_scene();
            uint32_t sky_id = 0;
            int ent_count = jce_state_get_entity_count();
            for (int ei = 0; ei < ent_count; ei++) {
                uint32_t eid = jce_state_get_entity_id_by_index(ei);
                if (eid == 0 || !jce_state_entity_exists(eid)) continue;
                if (scene && jce_scene_has_skybox(scene, (JceEntity)eid)) {
                    sky_id = eid;
                    break;
                }
            }

            jce_state_begin_batch_edit();
            if (!sky_id) {
                sky_id = jce_state_create_entity("Skybox", 0);
                if (sky_id)
                    jce_state_add_component(sky_id, JCE_COMP_FLAG_SKYBOX);
            }
            if (sky_id) {
                scene = jce_state_get_scene();
                JceSkyboxComponent *sky = scene
                    ? jce_scene_get_skybox(scene, (JceEntity)sky_id)
                    : NULL;
                if (sky) {
                    char rel_hdr[1024];
                    jce_editor_path_to_relative(rel_hdr, sizeof(rel_hdr),
                                                 asset_path);
                    const char *store_hdr = rel_hdr[0] ? rel_hdr : asset_path;
                    snprintf(sky->hdr_path, sizeof(sky->hdr_path),
                             "%s", store_hdr);
                    if (sky->exposure <= 0.0f)
                        sky->exposure = 1.0f;
                }
                jce_state_select_entity(sky_id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
                jce_editor_console_log(
                    "Applied HDR skybox: %s", asset_path);
            }
            jce_state_end_batch_edit();
            }
        }
        /* ── Texture / material dropped onto an entity ──────────── */
        else if (is_texture_or_material_asset(asset_path)) {
            uint32_t hit_id = pick_entity_at_mouse(screen_pos, avail);
            JceMeshRenderer *mesh_renderer_comp = find_mesh_renderer_component(hit_id);
            if (!mesh_renderer_comp) {
                /* No target — give the user explicit feedback instead
                 * of silently doing nothing. */
                jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                    "Texture '%s' was dropped on empty space — drop it "
                    "onto a mesh entity to assign it as a material slot, "
                    "or use an HDR (.hdr/.exr) file to set the skybox.",
                    asset_path);
            }
            if (mesh_renderer_comp) {
                bool is_mat_json = false;
                size_t path_len = strlen(asset_path);
                if (path_len >= 9 && strcmp(asset_path + path_len - 9, ".mat.json") == 0)
                    is_mat_json = true;

                bool applied = false;
                jce_state_begin_batch_edit();
                if (is_mat_json) {
                    applied = apply_material_asset_to_mesh_renderer(mesh_renderer_comp,
                                                                    asset_path);
                } else {
                    assign_texture_drop_to_mesh_renderer(mesh_renderer_comp,
                                                         detect_texture_drop_slot(asset_path),
                                                         asset_path);
                    applied = true;
                }
                jce_state_end_batch_edit();

                if (!applied) {
                    jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                        "Failed to apply material asset: %s", asset_path);
                    ImGui::EndDragDropTarget();
                    return;
                }

                jce_state_select_entity(hit_id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();

                const char *ent_name = jce_state_entity_name(hit_id);
                if (is_mat_json) {
                    jce_editor_console_log(
                        "Applied material '%s' to entity '%s'",
                        asset_path, ent_name ? ent_name : "?");
                } else {
                    jce_editor_console_log(
                        "Applied texture '%s' to entity '%s'",
                        asset_path, ent_name ? ent_name : "?");
                }
            }
        }
        /* ── Mesh dropped onto an existing entity ───────────────── */
        else if (is_mesh_asset(asset_path)) {
            uint32_t hit_id = pick_entity_at_mouse(screen_pos, avail);
            JceMeshRenderer *mesh_renderer_comp = find_mesh_renderer_component(hit_id);
            if (mesh_renderer_comp) {
                /* Replace the existing entity's mesh + extract material. */
                char rel_mesh[1024];
                jce_editor_path_to_relative(rel_mesh, sizeof(rel_mesh),
                                             asset_path);
                const char *store_mesh = rel_mesh[0] ? rel_mesh : asset_path;
                jce_state_begin_transient_edit();
                {
                    auto &mr = *mesh_renderer_comp;
                    snprintf(mr.mesh_path, sizeof(mr.mesh_path),
                             "%s", store_mesh);
                    mr.mesh_shape = 0;
                    mr.material_path[0] = '\0';
                    mr.albedo_tex[0] = '\0';
                    mr.mr_tex[0] = '\0';
                    mr.normal_tex[0] = '\0';
                    mr.ao_tex[0] = '\0';
                    mr.emissive_tex[0] = '\0';
                }
                jce_state_end_transient_edit();

                jce_editor_scene_asset_cache_queue_material_extract(
                    hit_id, asset_path, asset_path);

                jce_state_select_entity(hit_id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();

                const char *ent_name = jce_state_entity_name(hit_id);
                jce_editor_console_log(
                    "Replaced mesh on '%s' with '%s'",
                    ent_name ? ent_name : "?", asset_path);
            } else {
                /* ── Mesh dropped on empty space: create new entity ─ */
                float drop_pos[3] = { 0.0f, 0.0f, 0.0f };
                compute_ground_hit(screen_pos, avail, drop_pos);

                char name_buf[128];
                const char *fname  = asset_path;
                const char *sep    = strrchr(asset_path, '/');
                const char *sep2   = strrchr(asset_path, '\\');
                if (sep2 > sep) sep = sep2;
                if (sep) fname = sep + 1;
                snprintf(name_buf, sizeof(name_buf), "%s", fname);
                char *dot = strrchr(name_buf, '.');
                if (dot) *dot = '\0';

                {
                    char base[128];
                    snprintf(base, sizeof(base), "%s", name_buf);
                    int suffix = 1;
                    int total = jce_state_get_entity_count();
                    bool unique = false;
                    while (!unique) {
                        unique = true;
                        for (int ei = 0; ei < total; ei++) {
                            uint32_t eid = jce_state_get_entity_id_by_index(ei);
                            if (eid == 0 || !jce_state_entity_exists(eid)) continue;
                            const char *en = jce_state_entity_name(eid);
                            if (en && strcmp(en, name_buf) == 0) {
                                unique = false;
                                snprintf(name_buf, sizeof(name_buf),
                                         "%s_%d", base, suffix++);
                                break;
                            }
                        }
                    }
                }

                jce_state_begin_transient_edit();
                uint32_t id = create_default_scene_entity(name_buf, 0,
                                                          JCE_COMP_FLAG_MESH_RENDERER,
                                                          JCE_MESH_SHAPE_CUBE);
                if (id != 0) {
                    JceScene *scene = jce_state_get_scene();
                    if (scene) {
                        JceTransform *t = jce_scene_get_transform(scene, (JceEntity)id);
                        if (t) {
                            t->position.x = drop_pos[0];
                            t->position.y = drop_pos[1];
                            t->position.z = drop_pos[2];
                        }
                        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, (JceEntity)id);
                        if (mr) {
                            char rel_mesh2[1024];
                            jce_editor_path_to_relative(rel_mesh2, sizeof(rel_mesh2),
                                                         asset_path);
                            const char *store_mesh2 = rel_mesh2[0]
                                                       ? rel_mesh2 : asset_path;
                            snprintf(mr->mesh_path, sizeof(mr->mesh_path),
                                     "%s", store_mesh2);
                            mr->mesh_shape = 0;
                            mr->material_path[0] = '\0';
                            mr->albedo_tex[0] = '\0';
                            mr->mr_tex[0] = '\0';
                            mr->normal_tex[0] = '\0';
                            mr->ao_tex[0] = '\0';
                            mr->emissive_tex[0] = '\0';
                            jce_editor_console_log_level(JCE_CONSOLE_INFO,
                                "[drop-diag] new entity mesh_path='%s' exists=%d",
                                mr->mesh_path, (int)jce_fs_host_exists_file(mr->mesh_path));
                        }
                    }

                    jce_editor_scene_asset_cache_queue_material_extract(
                        id, asset_path, asset_path);

                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                    jce_editor_console_log(
                        "Dropped mesh '%s' into scene", name_buf);
                }
                jce_state_end_transient_edit();
            }
        }
        else {
            jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                "Unrecognized asset type for scene drop: %s", asset_path);
        }
    }

    ImGui::EndDragDropTarget();
}

/* ── Right-click context menu ────────────────────────────────────── */

static void draw_scene_context_menu(const SceneViewCtx *ctx)
{
    {
        ImVec2 mpos = ImGui::GetMousePos();
        bool mouse_in_vp = (mpos.x >= ctx->screen_pos.x &&
                            mpos.x <= ctx->screen_pos.x + ctx->avail.x &&
                            mpos.y >= ctx->screen_pos.y &&
                            mpos.y <= ctx->screen_pos.y + ctx->avail.y);

        if (mouse_in_vp && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
            && !ImGui::GetIO().KeyAlt)
        {
            ImGui::OpenPopup("SceneViewContextMenu");
        }
    }
    if (ImGui::BeginPopup("SceneViewContextMenu")) {
        uint32_t focused = jce_state_get_focused();
        int sel_count = 0;
        const uint32_t *sel_ids = jce_state_get_selection(&sel_count);
        bool has_selection = sel_count > 0;

        if (ImGui::BeginMenu(jce_editor_i18n("dialog.create"))) {
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createEmpty"))) {
                uint32_t id = create_default_scene_entity("New Entity", 0,
                                                          0,
                                                          JCE_MESH_SHAPE_CUBE);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
            }

            ImGui::Separator();

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create2D"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSprite"))) {
                    uint32_t id = create_default_scene_entity("Sprite", 0,
                                                              JCE_COMP_FLAG_SPRITE_RENDERER,
                                                              JCE_MESH_SHAPE_CUBE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createText"))) {
                    uint32_t id = create_default_scene_entity("UI Text", 0,
                                                              0,
                                                              JCE_MESH_SHAPE_CUBE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create3D"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCube"))) {
                    uint32_t id = create_default_scene_entity("Cube", 0,
                                                              JCE_COMP_FLAG_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_CUBE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere"))) {
                    uint32_t id = create_default_scene_entity("Sphere", 0,
                                                              JCE_COMP_FLAG_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_SPHERE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane"))) {
                    uint32_t id = create_default_scene_entity("Plane", 0,
                                                              JCE_COMP_FLAG_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_PLANE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCylinder"))) {
                    uint32_t id = create_default_scene_entity("Cylinder", 0,
                                                              JCE_COMP_FLAG_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_CYLINDER);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera"))) {
                uint32_t id = create_default_scene_entity("Camera", 0,
                                                          JCE_COMP_FLAG_CAMERA,
                                                          JCE_MESH_SHAPE_CUBE);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createLight"))) {
                uint32_t id = create_default_scene_entity("Light", 0,
                                                          JCE_COMP_FLAG_DIR_LIGHT,
                                                          JCE_MESH_SHAPE_CUBE);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
            }

            ImGui::EndMenu();
        }

        if (has_selection) {
            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.duplicate"), "Ctrl+D")) {
                if (sel_count > 1) {
                    uint32_t dup_ids[JCE_MAX_SELECTED];
                    int dup_count = 0;
                    int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                    jce_state_begin_batch_edit();
                    for (int i = 0; i < n; i++) {
                        uint32_t dup = jce_state_duplicate_entity(sel_ids[i]);
                        if (dup != 0 && dup_count < JCE_MAX_SELECTED)
                            dup_ids[dup_count++] = dup;
                    }
                    jce_state_end_batch_edit();
                    if (dup_count > 0) {
                        jce_state_select_entity(dup_ids[0], false);
                        for (int i = 1; i < dup_count; i++)
                            jce_state_select_entity(dup_ids[i], true);
                    }
                } else if (focused != 0) {
                    uint32_t dup = jce_state_duplicate_entity(focused);
                    if (dup != 0)
                        jce_state_select_entity(dup, false);
                }
                jce_editor_inspector_request_sync();
            }

            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.delete"), "Delete")) {
                uint32_t ids[JCE_MAX_SELECTED];
                int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                for (int si = 0; si < n; si++)
                    ids[si] = sel_ids[si];
                if (n > 0)
                    jce_editor_inspector_request_delete_confirm_many(ids, n);
            }

            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("scene.focusSelected"), "F")) {
                if (focused != 0) {
                    JceScene *scene = jce_state_get_scene();
                    JceTransform *t = scene
                        ? jce_scene_get_transform(scene, (JceEntity)focused)
                        : NULL;
                    if (t) {
                        jce_editor_scene_camera_set_target(
                            t->position.x, t->position.y, t->position.z);
                    }
                }
            }

            if (ImGui::BeginMenu(jce_editor_i18n("sceneView.gizmoMode"))) {
                JceGizmoMode menu_gm = jce_state_get_gizmo_mode();
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.translate"), "W", menu_gm == JCE_GIZMO_TRANSLATE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.rotate"), "E", menu_gm == JCE_GIZMO_ROTATE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.scale"), "R", menu_gm == JCE_GIZMO_SCALE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
                ImGui::EndMenu();
            }
        }

        ImGui::Separator();

        if (ImGui::BeginMenu(jce_editor_i18n("menu.view"))) {
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.resetCamera")))
                jce_editor_scene_camera_reset();
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.topView")))
                jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_TOP);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.frontView")))
                jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_FRONT);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.sideView")))
                jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_RIGHT);
            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }
}

/* ── Maya-style camera controls ──────────────────────────────────── */

static void handle_scene_camera_controls(bool viewport_hovered)
{
    ImGuiIO &io = ImGui::GetIO();
    bool alt_held = io.KeyAlt;
    /* Default: wheel up -> zoom in. scene_camera_zoom(positive) brings
       the camera closer, MouseWheel is positive on wheel-up, so the
       default sign is +1. The pref opts INTO Apple natural-scroll. */
    float dy_sign     = jce_editor_pref_invert_drag_y      ? -1.0f : 1.0f;
    float wheel_sign  = jce_editor_pref_invert_scroll_zoom ? -1.0f : 1.0f;

    if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        float dyaw   = io.MouseDelta.x * 0.005f;
        float dpitch = io.MouseDelta.y * 0.005f * dy_sign;
        jce_editor_scene_camera_orbit(dyaw, dpitch);
    }

    /* Pan: "grab the world" — drag right → world slides right under cursor
       (camera moves left). MouseDelta.x positive when dragging right;
       jce_editor_scene_camera_pan already moves the camera by (-dx, -dy)
       internally, so passing raw MouseDelta yields the grab semantic. */
    if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)) {
        float pan_x = io.MouseDelta.x;
        float pan_y = io.MouseDelta.y * dy_sign;
        jce_editor_scene_camera_pan(pan_x, pan_y);
    }

    if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f)) {
        float zoom_delta = -io.MouseDelta.y * 0.05f * dy_sign;
        jce_editor_scene_camera_zoom(zoom_delta);
    }

    if (viewport_hovered && fabsf(io.MouseWheel) > 0.0f) {
        jce_editor_scene_camera_zoom(io.MouseWheel * wheel_sign);
    }

    /* Touchpad two-finger horizontal swipe → 3D camera pan. The wheel
       handler already applied touchpad inversion if enabled, so the value
       here matches the user's chosen content-scroll convention. We negate
       once more so the world stays under the finger (camera moves the
       opposite direction). */
    if (viewport_hovered && fabsf(io.MouseWheelH) > 0.0f) {
        /* editor.cpp globally inverts io.MouseWheelH when
           touchpad_h_invert is enabled (to make ImGui windows scroll
           naturally). Scene viewport pan should follow the user's
           physical finger direction regardless of that ImGui-facing
           toggle, so undo the global flip here. */
        float wheel_h = io.MouseWheelH;
        if (jce_editor_pref_touchpad_h_invert) wheel_h = -wheel_h;
        float h_sign = jce_editor_pref_invert_scroll_zoom ? -1.0f : 1.0f;
        jce_editor_scene_camera_pan(-wheel_h * 8.0f * h_sign, 0.0f);
    }
}

/* ── Keyboard shortcuts ──────────────────────────────────────────── */

static void handle_scene_view_shortcuts(void)
{
    /* Shortcuts fire when the viewport is hovered (mouse is over it) OR
     * when it holds keyboard focus — matches Unity/UE editor behaviour.
     * WantTextInput guard prevents accidental triggers while typing in
     * any Inspector / Console field. */
    const bool want_text = ImGui::GetIO().WantTextInput;
    const bool active = !want_text &&
                        (ImGui::IsWindowFocused() ||
                         ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));
    if (active) {
        if (jce_hotkey_pressed(JCE_HK_GIZMO_TRANSLATE)) jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
        if (jce_hotkey_pressed(JCE_HK_GIZMO_ROTATE))    jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
        if (jce_hotkey_pressed(JCE_HK_GIZMO_SCALE))     jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);

        const bool frame_sel  = jce_hotkey_pressed(JCE_HK_VIEW_FRAME_SELECTED);
        const bool frame_all_ = jce_hotkey_pressed(JCE_HK_VIEW_FRAME_ALL);
        if (frame_sel || frame_all_) {
            scene_view_frame_entities(frame_all_);
        }

        if (jce_hotkey_pressed(JCE_HK_EDIT_DELETE)) {
            int dk = 0;
            const uint32_t *dids = jce_state_get_selection(&dk);
            if (dk > 0) {
                uint32_t ids[JCE_MAX_SELECTED];
                int n = dk < JCE_MAX_SELECTED ? dk : JCE_MAX_SELECTED;
                for (int di = 0; di < n; di++)
                    ids[di] = dids[di];
                jce_editor_inspector_request_delete_confirm_many(ids, n);
            }
        }

        if (jce_hotkey_pressed(JCE_HK_EDIT_DUPLICATE)) {
            int dk = 0;
            const uint32_t *dids = jce_state_get_selection(&dk);
            if (dk > 0) {
                uint32_t new_ids[JCE_MAX_SELECTED];
                int nc = 0;
                if (dk > 1)
                    jce_state_begin_batch_edit();
                for (int di = 0; di < dk && di < JCE_MAX_SELECTED; di++) {
                    uint32_t dup = jce_state_duplicate_entity(dids[di]);
                    if (dup != 0 && nc < JCE_MAX_SELECTED)
                        new_ids[nc++] = dup;
                }
                if (dk > 1)
                    jce_state_end_batch_edit();
                if (nc > 0) {
                    jce_state_select_entity(new_ids[0], false);
                    for (int di = 1; di < nc; di++)
                        jce_state_select_entity(new_ids[di], true);
                }
                jce_editor_inspector_request_sync();
            }
        }
    }
}

/* ── Overlays + picking helpers ──────────────────────────────────── */

/* Hit-test entities vs. the finished marquee rectangle. */
static void handle_marquee_selection(const SceneViewCtx *ctx,
                                     const float *view_mat,
                                     const float *proj_mat)
{
    if (!s_sel_pending) return;
    s_sel_pending = false;

    bool hit_any = false;
    bool add_mode = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
    if (!add_mode) jce_state_clear_selection();

    JceScene *scene = jce_state_get_scene();
    int total = jce_state_get_entity_count();
    for (int mi = 0; mi < total; mi++) {
        uint32_t meid = jce_state_get_entity_id_by_index(mi);
        if (meid == 0 || !jce_state_entity_exists(meid)) continue;
        if (!jce_state_entity_enabled(meid)) continue;

        JceTransform *t = scene ? jce_scene_get_transform(scene, (JceEntity)meid) : NULL;
        if (!t) continue;
        float mwp[3] = { t->position.x, t->position.y, t->position.z };
        float mws[3] = { t->scale.x,    t->scale.y,    t->scale.z    };

        float hx = fabsf(mws[0]) * 0.5f;
        float hy = fabsf(mws[1]) * 0.5f;
        float hz = fabsf(mws[2]) * 0.5f;
        if (hx < 0.1f) hx = 0.1f;
        if (hy < 0.1f) hy = 0.1f;
        if (hz < 0.1f) hz = 0.1f;

        static const float corners[8][3] = {
            {-1,-1,-1}, { 1,-1,-1}, {-1, 1,-1}, { 1, 1,-1},
            {-1,-1, 1}, { 1,-1, 1}, {-1, 1, 1}, { 1, 1, 1},
        };

        float bb_min_x =  1e30f, bb_min_y =  1e30f;
        float bb_max_x = -1e30f, bb_max_y = -1e30f;
        int projected = 0;

        for (int ci = 0; ci < 8; ci++) {
            float wx = mwp[0] + corners[ci][0] * hx;
            float wy = mwp[1] + corners[ci][1] * hy;
            float wz = mwp[2] + corners[ci][2] * hz;

            float mvx = view_mat[0]*wx + view_mat[4]*wy + view_mat[8] *wz + view_mat[12];
            float mvy = view_mat[1]*wx + view_mat[5]*wy + view_mat[9] *wz + view_mat[13];
            float mvz = view_mat[2]*wx + view_mat[6]*wy + view_mat[10]*wz + view_mat[14];
            float mvw = view_mat[3]*wx + view_mat[7]*wy + view_mat[11]*wz + view_mat[15];

            float mcx = proj_mat[0]*mvx + proj_mat[4]*mvy + proj_mat[8] *mvz + proj_mat[12]*mvw;
            float mcy = proj_mat[1]*mvx + proj_mat[5]*mvy + proj_mat[9] *mvz + proj_mat[13]*mvw;
            float mcw = proj_mat[3]*mvx + proj_mat[7]*mvy + proj_mat[11]*mvz + proj_mat[15]*mvw;
            if (mcw <= 0.0f) continue;

            float msx = ctx->screen_pos.x + (mcx / mcw + 1.0f) * 0.5f * ctx->avail.x;
            float msy = ctx->screen_pos.y + (1.0f - mcy / mcw) * 0.5f * ctx->avail.y;
            if (msx < bb_min_x) bb_min_x = msx;
            if (msx > bb_max_x) bb_max_x = msx;
            if (msy < bb_min_y) bb_min_y = msy;
            if (msy > bb_max_y) bb_max_y = msy;
            projected++;
        }

        if (projected <= 0) continue;

        if (bb_max_x >= s_sel_rect_min.x && bb_min_x <= s_sel_rect_max.x &&
            bb_max_y >= s_sel_rect_min.y && bb_min_y <= s_sel_rect_max.y)
        {
            hit_any = true;
            jce_state_select_entity(meid, true);
        }
    }
    jce_editor_inspector_request_sync();
    if (hit_any)
        jce_editor_layout_request_focus_inspector();
}

/* Single-click ray pick. */
static void handle_ray_pick(const SceneViewCtx *ctx,
                            const float *view_mat,
                            const float *proj_mat,
                            const float *eye)
{
    if (!s_sel_click_pending) return;
    s_sel_click_pending = false;

    bool add_mode = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;

    JceGizmoCamera pick_cam;
    memcpy(pick_cam.view, view_mat, sizeof(float) * 16);
    memcpy(pick_cam.proj, proj_mat, sizeof(float) * 16);
    memcpy(pick_cam.eye,  eye,      sizeof(float) * 3);
    pick_cam.viewport_size[0]   = ctx->avail.x;
    pick_cam.viewport_size[1]   = ctx->avail.y;
    pick_cam.viewport_origin[0] = ctx->screen_pos.x;
    pick_cam.viewport_origin[1] = ctx->screen_pos.y;

    float ray_o[3], ray_d[3];
    gm_screen_to_ray(&pick_cam, s_sel_click_pos.x,
                     s_sel_click_pos.y, ray_o, ray_d);

    uint32_t best_id = 0;
    float    best_t  = 1e30f;

    JceScene *scene = jce_state_get_scene();
    int total = jce_state_get_entity_count();
    for (int pi = 0; pi < total; pi++) {
        uint32_t pid = jce_state_get_entity_id_by_index(pi);
        if (pid == 0 || !jce_state_entity_exists(pid)) continue;
        if (!jce_state_entity_enabled(pid)) continue;

        JceTransform *xf = scene ? jce_scene_get_transform(scene, (JceEntity)pid) : NULL;
        if (!xf) continue;
        float pos[3] = { xf->position.x, xf->position.y, xf->position.z };
        float scl[3] = { xf->scale.x,    xf->scale.y,    xf->scale.z    };

        float hx = fabsf(scl[0]) * 0.5f;
        float hy = fabsf(scl[1]) * 0.5f;
        float hz = fabsf(scl[2]) * 0.5f;
        if (hx < 0.1f) hx = 0.1f;
        if (hy < 0.1f) hy = 0.1f;
        if (hz < 0.1f) hz = 0.1f;

        float bmin[3] = { pos[0]-hx, pos[1]-hy, pos[2]-hz };
        float bmax[3] = { pos[0]+hx, pos[1]+hy, pos[2]+hz };

        float t;
        jce_vec3 ro = jce_v3(ray_o[0], ray_o[1], ray_o[2]);
        jce_vec3 rd = jce_v3(ray_d[0], ray_d[1], ray_d[2]);
        jce_vec3 bmin_v = jce_v3(bmin[0], bmin[1], bmin[2]);
        jce_vec3 bmax_v = jce_v3(bmax[0], bmax[1], bmax[2]);
        if (jce_ray_aabb_intersect(ro, rd, bmin_v, bmax_v, &t) && t >= 0.0f) {
            if (t < best_t) {
                best_t  = t;
                best_id = pid;
            }
        }
    }

    if (best_id != 0) {
        if (add_mode && jce_state_is_selected(best_id))
            jce_state_deselect_entity(best_id);
        else
            jce_state_select_entity(best_id, add_mode);
        jce_editor_inspector_request_sync();
        jce_editor_layout_request_focus_inspector();
    } else if (!add_mode) {
        jce_state_clear_selection();
        jce_editor_inspector_request_sync();
    }
}

/* Orange diamond outline on all selected entities. */
static void draw_selection_outlines(const SceneViewCtx *ctx,
                                    const float *view_mat,
                                    const float *proj_mat)
{
    int sel_count = 0;
    const uint32_t *sel_ids = jce_state_get_selection(&sel_count);
    JceScene *scene = jce_state_get_scene();
    for (int si = 0; si < sel_count; si++) {
        uint32_t sid = sel_ids[si];
        if (sid == 0 || !jce_state_entity_exists(sid)) continue;
        if (!jce_state_entity_enabled(sid)) continue;
        JceTransform *t = scene ? jce_scene_get_transform(scene, (JceEntity)sid) : NULL;
        if (!t) continue;
        float swp[3] = { t->position.x, t->position.y, t->position.z };

        float svx = view_mat[0]*swp[0] + view_mat[4]*swp[1] + view_mat[8] *swp[2] + view_mat[12];
        float svy = view_mat[1]*swp[0] + view_mat[5]*swp[1] + view_mat[9] *swp[2] + view_mat[13];
        float svz = view_mat[2]*swp[0] + view_mat[6]*swp[1] + view_mat[10]*swp[2] + view_mat[14];
        float svw = view_mat[3]*swp[0] + view_mat[7]*swp[1] + view_mat[11]*swp[2] + view_mat[15];
        float scx = proj_mat[0]*svx + proj_mat[4]*svy + proj_mat[8] *svz + proj_mat[12]*svw;
        float scy = proj_mat[1]*svx + proj_mat[5]*svy + proj_mat[9] *svz + proj_mat[13]*svw;
        float scw = proj_mat[3]*svx + proj_mat[7]*svy + proj_mat[11]*svz + proj_mat[15]*svw;
        if (scw <= 0.0f) continue;

        float ssx = ctx->screen_pos.x + (scx / scw + 1.0f) * 0.5f * ctx->avail.x;
        float ssy = ctx->screen_pos.y + (1.0f - scy / scw) * 0.5f * ctx->avail.y;

        if (ssx < ctx->screen_pos.x || ssx > ctx->screen_pos.x + ctx->avail.x ||
            ssy < ctx->screen_pos.y || ssy > ctx->screen_pos.y + ctx->avail.y)
            continue;

        float mr = 14.0f;
        ctx->dl->AddQuadFilled(
            ImVec2(ssx,      ssy - mr),
            ImVec2(ssx + mr, ssy),
            ImVec2(ssx,      ssy + mr),
            ImVec2(ssx - mr, ssy),
            IM_COL32(255, 120, 0, 60));
        ctx->dl->AddQuad(
            ImVec2(ssx,      ssy - mr),
            ImVec2(ssx + mr, ssy),
            ImVec2(ssx,      ssy + mr),
            ImVec2(ssx - mr, ssy),
            IM_COL32(255, 120, 0, 230), 2.0f);
    }
}

/* ── Overlays + picking (axis indicator, view cube, marquee, ray) ── */

static void draw_scene_overlays_and_pick(const SceneViewCtx *ctx)
{
    float view_mat[16], proj_mat[16], eye[3];
    if (!jce_editor_scene_get_camera_matrices(view_mat, proj_mat, eye,
                                              ctx->avail.x, ctx->avail.y))
        return;

    JceGizmoCamera overlay_cam;
    memcpy(overlay_cam.view, view_mat, sizeof(float) * 16);
    memcpy(overlay_cam.proj, proj_mat, sizeof(float) * 16);
    memcpy(overlay_cam.eye,  eye,      sizeof(float) * 3);
    overlay_cam.viewport_size[0]   = ctx->avail.x;
    overlay_cam.viewport_size[1]   = ctx->avail.y;
    overlay_cam.viewport_origin[0] = ctx->screen_pos.x;
    overlay_cam.viewport_origin[1] = ctx->screen_pos.y;

    if (jce_state_show_flag(JCE_SHOW_FLAG_LIGHT_ICONS) ||
        jce_state_show_flag(JCE_SHOW_FLAG_CAMERA_ICONS))
        draw_scene_helper_icons(ctx->dl, &overlay_cam);

    int axis_click = -1;
    if (jce_state_show_flag(JCE_SHOW_FLAG_WORLD_AXIS))
        axis_click = draw_axis_indicator(ctx->dl, ctx->screen_pos, ctx->avail, view_mat);
    int cube_click = draw_view_cube(ctx->dl, ctx->screen_pos, ctx->avail, view_mat);

    if (axis_click >= 0) {
        static const JceCamPresetView axis_presets[6] = {
            JCE_CAM_VIEW_RIGHT, JCE_CAM_VIEW_TOP, JCE_CAM_VIEW_FRONT,
            JCE_CAM_VIEW_LEFT, JCE_CAM_VIEW_BOTTOM, JCE_CAM_VIEW_BACK,
        };
        jce_editor_scene_camera_snap_view(axis_presets[axis_click]);
    }

    if (cube_click == -2) {
        jce_editor_scene_camera_reset();
    } else if (cube_click >= 0) {
        static const JceCamPresetView cube_presets[6] = {
            JCE_CAM_VIEW_FRONT, JCE_CAM_VIEW_BACK,
            JCE_CAM_VIEW_TOP, JCE_CAM_VIEW_BOTTOM,
            JCE_CAM_VIEW_RIGHT, JCE_CAM_VIEW_LEFT,
        };
        jce_editor_scene_camera_snap_view(cube_presets[cube_click]);
    }

    handle_marquee_selection(ctx, view_mat, proj_mat);
    handle_ray_pick(ctx, view_mat, proj_mat, eye);
    draw_selection_outlines(ctx, view_mat, proj_mat);
}

/* ── Content (embeddable in tabs) ────────────────────────────────── */

void jce_editor_panel_scene_view_content(void)
{
    flush_async_drop_material_extracts();
    draw_scene_view_toolbar();

    SceneViewCtx ctx;
    if (!setup_scene_viewport(&ctx))
        return;

    draw_scene_context_menu(&ctx);
    handle_scene_camera_controls(ctx.viewport_hovered);

    /* ── Terrain brush (Phase 2-B.2) ─────────────────────────────
     *  Active only when the Terrain panel arms it. Steals LMB from
     *  selection so a click/drag inside the viewport raycasts onto
     *  the active terrain and applies the current brush at the
     *  hit point. Plain LMB only — Alt-LMB still orbits the camera. */
    bool brush_consumed = false;
    if (ctx.viewport_hovered && jce_terrain_panel_brush_armed() &&
        !ImGui::GetIO().KeyAlt &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseDown(ImGuiMouseButton_Left)))
    {
        float vmat[16], pmat[16], eye[3];
        if (jce_editor_scene_get_camera_matrices(vmat, pmat, eye,
                                                 ctx.avail.x, ctx.avail.y)) {
            JceGizmoCamera cam;
            memcpy(cam.view, vmat, sizeof vmat);
            memcpy(cam.proj, pmat, sizeof pmat);
            memcpy(cam.eye,  eye,  sizeof eye);
            cam.viewport_size[0]   = ctx.avail.x;
            cam.viewport_size[1]   = ctx.avail.y;
            cam.viewport_origin[0] = ctx.screen_pos.x;
            cam.viewport_origin[1] = ctx.screen_pos.y;
            ImVec2 m = ImGui::GetMousePos();
            float ro[3], rd[3];
            gm_screen_to_ray(&cam, m.x, m.y, ro, rd);

            float hit[3];
            JceTerrain *terr = jce_terrain_panel_get_terrain();
            bool got = false;
            if (terr && jce_terrain_raycast(terr, ro, rd, 10000.0f, hit))
                got = true;
            if (!got && fabsf(rd[1]) > 1e-6f) {
                /* Fallback: hit Y=0 plane. */
                float t = -ro[1] / rd[1];
                if (t > 0.0f) {
                    hit[0] = ro[0] + rd[0] * t;
                    hit[1] = 0.0f;
                    hit[2] = ro[2] + rd[2] * t;
                    got = true;
                }
            }
            if (got) {
                jce_terrain_panel_apply_brush_world(hit[0], hit[2],
                                                     ImGui::GetIO().DeltaTime);
                brush_consumed = true;
            }
        }
    }

    if (!brush_consumed)
        handle_scene_selection_box(&ctx);
    else
        (void)0;
    if (jce_state_show_flag(JCE_SHOW_FLAG_GIZMOS))
        update_and_draw_scene_gizmo(&ctx);

    if (s_gizmo_history_batch_open && !jce_gizmo_is_active()) {
        jce_state_end_batch_edit();
        s_gizmo_history_batch_open = false;
    }

    handle_scene_view_shortcuts();
    draw_scene_overlays_and_pick(&ctx);
}

/* ── Standalone wrapper ──────────────────────────────────────────── */

void jce_editor_panel_scene_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW);
    if (!*vis) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    char title[256];
    snprintf(title, sizeof(title), "%s###scene_view", jce_editor_i18n("Scene"));
    if (ImGui::Begin(title, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_scene_view_content();
    ImGui::End();
    ImGui::PopStyleVar();

    /* Camera Preview: shown when selected entity has a Camera component. */
    {
        JceScene *scene = jce_state_get_scene();
        uint32_t focused = jce_state_get_focused();
        if (scene && focused != 0) {
            uint64_t cf = jce_scene_get_component_flags(scene, (JceEntity)focused);
            if (cf & JCE_COMP_FLAG_CAMERA) {
                JceCameraComponent *cam = jce_scene_get_camera(scene, (JceEntity)focused);
                if (cam) {
                    ImGui::SetNextWindowSize(ImVec2(280.0f, 180.0f), ImGuiCond_FirstUseEver);
                    ImGui::SetNextWindowBgAlpha(0.85f);
                    char win_title[128];
                    snprintf(win_title, sizeof(win_title), "%s##jce_cam_preview",
                             jce_editor_i18n("inspector.camera.preview.title"));
                    if (ImGui::Begin(win_title, nullptr,
                                     ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoCollapse)) {
                        static const char *clear_i18n[] = {
                            "inspector.camera.clearMode.skybox",
                            "inspector.camera.clearMode.color",
                            "inspector.camera.clearMode.depthOnly",
                            "inspector.camera.clearMode.nothing",
                        };
                        int cm = (int)cam->clear_mode;
                        if (cm < 0 || cm > 3) cm = 0;
                        ImGui::Text(jce_editor_i18n("inspector.camera.preview.fov"),
                                    (double)cam->fov_deg);
                        ImGui::Text(jce_editor_i18n("inspector.camera.preview.nearFar"),
                                    (double)cam->near_plane, (double)cam->far_plane);
                        ImGui::Text(jce_editor_i18n("inspector.camera.preview.clear"),
                                    jce_editor_i18n(clear_i18n[cm]));
                        ImGui::Text(jce_editor_i18n("inspector.camera.preview.stack"),
                                    (int)cam->stack_index,
                                    cam->is_primary
                                        ? jce_editor_i18n("inspector.camera.preview.stackPrimary")
                                        : "");
                        ImGui::TextDisabled("%s",
                                    jce_editor_i18n("inspector.camera.preview.noTarget"));
                    }
                    ImGui::End();
                }
            }
        }
    }
}
