/*
 * jce_panel_scene_view.cpp  Scene View panel — toolbar, camera, context menu,
 *                           shortcuts, overlays, main content.
 *
 * Helper icons & entity creation are in jce_scene_view_helpers.cpp.
 * Axis indicator & view cube are in jce_scene_view_cube.cpp.
 * Gizmo interaction & selection box are in jce_scene_view_gizmo.cpp.
 */

#include "jce_scene_view_internal.h"
#include <jce/os/core/jce_filesystem.h>   /* host write: editor code does not use stdio file IO */
#include <cstdlib>      /* getenv */
#include <cstdio>       /* snprintf */
#include "jce_panel_common.h"        /* multi-select duplicate / delete */
#include "ui/jce_editor_dnd.h"
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_i18n.h"
#include "scene/jce_editor_scene_asset_cache.h"
#include "scene/jce_editor_scene_camera_tools.h"
#include "core/jce_hotkeys.h"
#include "core/jce_editor_config.h"
#include "core/jce_editor_project_state.h"
#include "ui/jce_editor_panels.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>   /* jce_free for the import GLB buffer */
#include <jce/os/core/jce_log.h>     /* JCE_DBG_PICK QA hook logging */

extern "C" {
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_model.h>
#include <jce/middleware/scene/jce_terrain.h>

bool jce_terrain_panel_brush_armed(void);
struct JceTerrain *jce_terrain_panel_get_terrain(void);
void jce_terrain_panel_apply_brush_world(float wx, float wz, float dt);
void jce_terrain_panel_end_brush_stroke(void);

/* Foliage density paint brush (large-world #8a) — same scene-view raycast path. */
bool jce_foliage_brush_armed(void);
void jce_foliage_brush_apply_world(float wx, float wz, float dt);
void jce_foliage_brush_end_stroke(void);
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
    jce_editor::help_tip(jce_editor_i18n("sceneView.tooltip.translate"));
    ImGui::SameLine();
    if (ImGui::RadioButton("R", gm == JCE_GIZMO_ROTATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    jce_editor::help_tip(jce_editor_i18n("sceneView.tooltip.rotate"));
    ImGui::SameLine();
    if (ImGui::RadioButton("S", gm == JCE_GIZMO_SCALE))
        jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
    jce_editor::help_tip(jce_editor_i18n("sceneView.tooltip.scale"));
    ImGui::SameLine();
    bool pivot_edit = jce_state_get_pivot_edit_mode();
    if (ImGui::RadioButton("D", pivot_edit))
        jce_state_set_pivot_edit_mode(!pivot_edit);
    jce_editor::help_tip(jce_editor_i18n("sceneView.tooltip.pivotEdit"));

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

    /* Snap-increment settings (applied while Ctrl-dragging a gizmo). */
    if (ImGui::Button(jce_editor_i18n("sceneView.snap.button")))
        ImGui::OpenPopup("##SnapSettings");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("sceneView.snap.tooltip"));
    if (ImGui::BeginPopup("##SnapSettings")) {
        ImGui::TextUnformatted(jce_editor_i18n("sceneView.snap.title"));
        ImGui::Separator();
        /* Persistent toggle: snap without holding Ctrl (Ctrl still forces it). */
        bool snap_on = jce_state_get_gizmo_snap_enabled();
        if (ImGui::Checkbox(jce_editor_i18n_id("sceneView.snap.enabled",
                            "Snap enabled (no Ctrl needed)"), &snap_on))
            jce_state_set_gizmo_snap_enabled(snap_on);
        ImGui::PushItemWidth(120.0f);

        float snap_t = jce_state_get_gizmo_snap_translate();
        if (ImGui::InputFloat(jce_editor_i18n("sceneView.snap.move"),
                              &snap_t, 0.05f, 0.5f, "%.3f"))
            jce_state_set_gizmo_snap_translate(snap_t);

        float snap_r = jce_state_get_gizmo_snap_rotate();
        if (ImGui::InputFloat(jce_editor_i18n("sceneView.snap.rotate"),
                              &snap_r, 1.0f, 5.0f, "%.2f"))
            jce_state_set_gizmo_snap_rotate(snap_r);

        float snap_s = jce_state_get_gizmo_snap_scale();
        if (ImGui::InputFloat(jce_editor_i18n("sceneView.snap.scale"),
                              &snap_s, 0.05f, 0.25f, "%.3f"))
            jce_state_set_gizmo_snap_scale(snap_s);

        ImGui::PopItemWidth();
        ImGui::Separator();
        if (ImGui::Button(jce_editor_i18n("sceneView.snap.reset"))) {
            jce_state_set_gizmo_snap_translate(0.5f);
            jce_state_set_gizmo_snap_rotate(15.0f);
            jce_state_set_gizmo_snap_scale(0.25f);
        }
        ImGui::EndPopup();
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

    if (ImGui::Button(jce_editor_i18n_or("sceneView.overview", "Overview"))) {
        jce_editor_scene_frame_overview();
    }
    jce_editor::help_tip(
        jce_editor_i18n_or("sceneView.overview.tooltip",
                           "Frame the whole world from above"));

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
            ImGui::Separator();
            /* Debug channel views (unlit material channels via fs_pbr). */
            if (ImGui::MenuItem(jce_editor_i18n_id("sceneView.normals", "Normals"), NULL, vm == JCE_VIEW_NORMALS))
                jce_state_set_view_mode(JCE_VIEW_NORMALS);
            if (ImGui::MenuItem(jce_editor_i18n_id("sceneView.roughness", "Roughness"), NULL, vm == JCE_VIEW_ROUGHNESS))
                jce_state_set_view_mode(JCE_VIEW_ROUGHNESS);
            if (ImGui::MenuItem(jce_editor_i18n_id("sceneView.metallic", "Metallic"), NULL, vm == JCE_VIEW_METALLIC))
                jce_state_set_view_mode(JCE_VIEW_METALLIC);
            if (ImGui::MenuItem(jce_editor_i18n_id("sceneView.ao", "Ambient Occlusion"), NULL, vm == JCE_VIEW_AO))
                jce_state_set_view_mode(JCE_VIEW_AO);
            ImGui::Separator();
            /* Shadow + depth views.  Separated from the material channels
             * above because they answer a different kind of question: those
             * show what a surface IS, these show where it sits relative to the
             * shadow cascades.  Read together they say whether a change that
             * tracks distance is a cascade boundary, the end of the shadow
             * range, or neither. */
            if (ImGui::MenuItem(jce_editor_i18n_id("sceneView.sceneDepth", "Scene Depth"), NULL, vm == JCE_VIEW_SCENE_DEPTH))
                jce_state_set_view_mode(JCE_VIEW_SCENE_DEPTH);
            if (ImGui::MenuItem(jce_editor_i18n_id("sceneView.shadowCascades", "Shadow Cascades"), NULL, vm == JCE_VIEW_SHADOW_CASCADES))
                jce_state_set_view_mode(JCE_VIEW_SHADOW_CASCADES);
            if (ImGui::MenuItem(jce_editor_i18n_id("sceneView.shadowMask", "Shadow Mask"), NULL, vm == JCE_VIEW_SHADOW_MASK))
                jce_state_set_view_mode(JCE_VIEW_SHADOW_MASK);
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
                { "sceneView.flag.particleIcons", JCE_SHOW_FLAG_PARTICLE_ICONS },
                { "sceneView.flag.colliders",     JCE_SHOW_FLAG_COLLIDERS      },
                { "sceneView.flag.skybox",        JCE_SHOW_FLAG_SKYBOX         },
                { "sceneView.flag.boundingBoxes", JCE_SHOW_FLAG_BOUNDING_BOXES },
                { "sceneView.flag.worldAxis",     JCE_SHOW_FLAG_WORLD_AXIS     },
                { "sceneView.flag.statsOverlay",  JCE_SHOW_FLAG_STATS_OVERLAY  },
                { "sceneView.flag.navMesh",       JCE_SHOW_FLAG_NAVMESH        },
                { "sceneView.flag.streaming",     JCE_SHOW_FLAG_STREAMING     },
                { "sceneView.flag.ui",            JCE_SHOW_FLAG_UI             },
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
              | JCE_SHOW_FLAG_WORLD_AXIS | JCE_SHOW_FLAG_PARTICLE_ICONS
              | JCE_SHOW_FLAG_UI);
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

        /* Camera bookmarks: capture/restore exact viewpoints — invaluable for
         * navigating a large world (jump between work areas).  Stored per
         * scene in the project state ("bookmark.<n>" = "x,y,z,yaw,pitch,dist")
         * so a bookmark set in one scene never teleports the camera in
         * another, and survives restarts.  Inert without an open project. */
        if (ImGui::BeginMenu(jce_editor_i18n_id("sceneView.bookmarks", "Camera Bookmarks"))) {
            const char *bm_scene = jce_state_get_current_scene_path();
            const bool  bm_live  = jce_editor_pstate_active() &&
                                   bm_scene && bm_scene[0];
            char bm_key[16];
            char bm_val[128];
            for (int i = 0; i < 9; i++) {
                snprintf(bm_key, sizeof bm_key, "bookmark.%d", i + 1);
                bool valid = bm_live && jce_editor_pstate_scene_get_str(
                    bm_scene, bm_key, bm_val, sizeof bm_val);
                char lbl[80];
                snprintf(lbl, sizeof lbl, "%s %d%s",
                         jce_editor_i18n_id("sceneView.goToBookmark", "Go to bookmark"),
                         i + 1, valid ? "" : " (empty)");
                if (ImGui::MenuItem(lbl, NULL, false, valid)) {
                    float t[3], yaw, pitch, dist;
                    if (sscanf(bm_val, "%f,%f,%f,%f,%f,%f",
                               &t[0], &t[1], &t[2], &yaw, &pitch, &dist) == 6)
                        jce_editor_scene_camera_set_state(t, yaw, pitch, dist);
                }
            }
            ImGui::Separator();
            for (int i = 0; i < 9; i++) {
                char lbl[80];
                snprintf(lbl, sizeof lbl, "%s %d",
                         jce_editor_i18n_id("sceneView.setBookmark", "Set bookmark"), i + 1);
                if (ImGui::MenuItem(lbl, NULL, false, bm_live)) {
                    float t[3], yaw, pitch, dist;
                    jce_editor_scene_camera_get_state(t, &yaw, &pitch, &dist);
                    snprintf(bm_key, sizeof bm_key, "bookmark.%d", i + 1);
                    snprintf(bm_val, sizeof bm_val,
                             "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                             t[0], t[1], t[2], yaw, pitch, dist);
                    jce_editor_pstate_scene_set_str(bm_scene, bm_key, bm_val);
                }
            }
            ImGui::EndMenu();
        }

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

    /* JCE_DBG_VIEWPORT_RECT=<path> -- write where the 3D view sits in the
     * window, in pixels.
     *
     * The capture harness screenshots the whole editor, so every measurement
     * taken from one has to know which pixels are the render and which are
     * chrome.  Guessing that rectangle cost this investigation real time and
     * two wrong conclusions: a "far ground" region that turned out to be
     * mostly sky, and a near-to-far colour gradient measured along IMAGE ROWS,
     * which on a tilted view is not distance at all -- the far hillside also
     * faces the sun differently, so the two effects were inseparable.
     *
     * The panel knows the rectangle exactly. Writing it down is cheaper than
     * any amount of inferring it from pixels, and it cannot drift from the
     * layout because it is emitted by the layout. */
    if (const char *rp = std::getenv("JCE_DBG_VIEWPORT_RECT")) {
        static bool s_written = false;
        if (!s_written && avail.x > 1.0f && avail.y > 1.0f) {
            s_written = true;
            char line[192];
            int n = std::snprintf(line, sizeof line,
                "VIEWPORT x=%d y=%d w=%d h=%d\n",
                (int)screen_pos.x, (int)screen_pos.y,
                (int)avail.x, (int)avail.y);
            if (n > 0) jce_fs_host_write_all(rp, line, (size_t)n);
        }
    }

    dl->AddRectFilled(screen_pos,
                      ImVec2(screen_pos.x + avail.x, screen_pos.y + avail.y),
                      IM_COL32(30, 30, 40, 255));

    jce_editor_scene_render_frame(vp_w, vp_h);

    uint16_t tex_idx = jce_editor_scene_render_get_texture();
    if (tex_idx != UINT16_MAX) {
        const bool origin_bl = jce_renderer_origin_bottom_left();
        if (origin_bl) {
            ImGui::Image((ImTextureID)(uintptr_t)((uint32_t)tex_idx + 1u), avail,
                         ImVec2(0.0f, uv_v1), ImVec2(uv_u1, 0.0f));
        } else {
            ImGui::Image((ImTextureID)(uintptr_t)((uint32_t)tex_idx + 1u), avail,
                         ImVec2(0.0f, 0.0f), ImVec2(uv_u1, uv_v1));
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(screen_pos.x + 8, screen_pos.y + 8));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.6f),
            "%s  %.0f x %.0f", jce_editor_i18n("Scene"), avail.x, avail.y);
    }

    ImGui::SetCursorScreenPos(screen_pos);
    ImGui::InvisibleButton("##SceneViewInput", avail,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    handle_scene_view_asset_drop(screen_pos, avail);  /* drop target must follow the button immediately */
    bool viewport_hovered       = ImGui::IsItemHovered();
    bool viewport_active        = ImGui::IsItemActive();
    bool viewport_left_clicked  = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool viewport_right_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    ctx->avail            = avail;
    ctx->screen_pos       = screen_pos;
    ctx->dl               = dl;
    ctx->viewport_hovered = viewport_hovered;
    ctx->viewport_active  = viewport_active;
    ctx->viewport_left_clicked  = viewport_left_clicked;
    ctx->viewport_right_clicked = viewport_right_clicked;
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

/* Forward decl of the engine bundle mesh converter (jce_bundle_mesh_convert.cpp,
 * linked into the editor via the jce_resource layer). */
extern "C" int jce_bundle_convert_to_glb(const uint8_t *src, size_t src_sz,
                                         const char *ext_hint,
                                         uint8_t **out_buf, size_t *out_size);

/* Inc 2a — convert-on-import: a model dragged into the scene is normalised to a
 * sibling ".glb" so the project stays single-format (the runtime mesh loader is
 * cgltf-only; a raw .obj would render in-editor via assimp but silently fail at
 * runtime).  Already-glTF inputs pass through; an existing sibling .glb is
 * reused; conversion failure falls back to the original path (the editor's
 * assimp preview still renders it).  Returns a pointer into `out` (the .glb
 * path) or the original `asset_path`. */
static const char *import_ensure_glb(const char *asset_path, char *out, size_t cap)
{
    if (!asset_path || !asset_path[0]) return asset_path;
    char ext[16];
    copy_ext_lower(asset_path, ext, sizeof(ext));
    if (ext[0] == '\0' || strcmp(ext, ".glb") == 0 || strcmp(ext, ".gltf") == 0)
        return asset_path;                 /* already glTF / no extension */

    const char *dot = strrchr(asset_path, '.');
    size_t stem = dot ? (size_t)(dot - asset_path) : strlen(asset_path);
    if (stem + 5 > cap) return asset_path;
    memcpy(out, asset_path, stem);
    memcpy(out + stem, ".glb", 5);         /* incl NUL */

    if (jce_fs_host_exists_file(out))
        return out;                        /* converted on a previous drop */

    uint64_t in_sz = 0;
    void    *in_buf = jce_fs_host_read_all(asset_path, &in_sz);
    if (!in_buf || in_sz == 0) {
        if (in_buf) jce_fs_buffer_free(in_buf);
        return asset_path;
    }
    uint8_t *glb = NULL;
    size_t   glb_sz = 0;
    int ok = jce_bundle_convert_to_glb((const uint8_t *)in_buf, (size_t)in_sz,
                                       dot ? dot + 1 : "", &glb, &glb_sz);
    jce_fs_buffer_free(in_buf);
    if (!ok || !glb || glb_sz == 0) {
        if (glb) jce_free(glb);
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "Import: GLB conversion failed for '%s' (using source)", asset_path);
        return asset_path;
    }
    bool wrote = jce_fs_host_write_all(out, glb, glb_sz);
    jce_free(glb);
    if (!wrote) return asset_path;
    jce_editor_console_log_level(JCE_CONSOLE_INFO,
        "Import: converted model to glTF -> %s", out);
    return out;
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
    const char *name = jce_editor_path_basename_view(path ? path : "");

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
    const char *store_path = jce_editor_path_relative_or(rel, sizeof(rel), asset_path);

    auto &mr = *mesh_renderer_comp;
    switch (slot) {
    case 1:
        mr.mr_tex = jce_scene_intern(jce_state_get_scene(), store_path);
        break;
    case 2:
        mr.normal_tex = jce_scene_intern(jce_state_get_scene(), store_path);
        break;
    case 3:
        mr.ao_tex = jce_scene_intern(jce_state_get_scene(), store_path);
        break;
    case 4:
        mr.emissive_tex = jce_scene_intern(jce_state_get_scene(), store_path);
        break;
    case 0:
    default:
        mr.albedo_tex = jce_scene_intern(jce_state_get_scene(), store_path);
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
    const char *store_path = jce_editor_path_relative_or(rel, sizeof(rel), asset_path);

    auto &mr = *mesh_renderer_comp;
    mr.material_path = jce_scene_intern(jce_state_get_scene(), store_path);
    mr.albedo_tex = jce_scene_intern(jce_state_get_scene(), "");
    mr.mr_tex = jce_scene_intern(jce_state_get_scene(), "");
    mr.normal_tex = jce_scene_intern(jce_state_get_scene(), "");
    mr.ao_tex = jce_scene_intern(jce_state_get_scene(), "");
    mr.emissive_tex = jce_scene_intern(jce_state_get_scene(), "");

    if (tex_paths[0][0])
        mr.albedo_tex = jce_scene_intern(jce_state_get_scene(), tex_paths[0]);
    if (tex_paths[1][0])
        mr.mr_tex = jce_scene_intern(jce_state_get_scene(), tex_paths[1]);
    if (tex_paths[2][0])
        mr.normal_tex = jce_scene_intern(jce_state_get_scene(), tex_paths[2]);
    if (tex_paths[3][0])
        mr.ao_tex = jce_scene_intern(jce_state_get_scene(), tex_paths[3]);
    if (tex_paths[4][0])
        mr.emissive_tex = jce_scene_intern(jce_state_get_scene(), tex_paths[4]);

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

    /* The importer reports extracted-texture files CWD-relative; convert
     * to the canonical project-relative form before storing, or the saved
     * scene carries references no resolver (or the bundle packer) can
     * anchor once the editor runs from a different directory. */
    auto &mr = *mesh_renderer_comp;
    if (material->albedo_tex[0])
        jce_editor_path_store_asset_ref_interned(jce_state_get_scene(), &mr.albedo_tex, material->albedo_tex);
    if (material->mr_tex[0])
        jce_editor_path_store_asset_ref_interned(jce_state_get_scene(), &mr.mr_tex, material->mr_tex);
    if (material->normal_tex[0])
        jce_editor_path_store_asset_ref_interned(jce_state_get_scene(), &mr.normal_tex, material->normal_tex);
    if (material->ao_tex[0])
        jce_editor_path_store_asset_ref_interned(jce_state_get_scene(), &mr.ao_tex, material->ao_tex);
    if (material->emissive_tex[0])
        jce_editor_path_store_asset_ref_interned(jce_state_get_scene(), &mr.emissive_tex, material->emissive_tex);

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

/* ── The coarse, SYNCHRONOUS picker ──────────────────────────────────
 * Ray-test every enabled entity against the box built from its transform
 * POSITION and SCALE (a unit cube scaled by the transform, floored at 0.1 per
 * axis) and report the nearest hit.  Deliberately coarse: it ignores the
 * mesh's real bounds, so a glTF model whose geometry is much larger or
 * smaller than its transform scale is hit at the wrong extent.
 *
 * Why it exists next to the pixel-accurate GPU object-id pick
 * (jce_editor_scene_pick_* → engine jce_scene_pick.c): that pick is
 * ASYNCHRONOUS by contract — request() only records the pixel, an ID render
 * services it on a later frame, poll() answers frames after that (see the
 * s_gpu_pick_selection_pending state machine in handle_ray_pick), and only
 * one request may be in flight.  Click and marquee selection CAN wait across
 * frames, so they use it and fall back here only when it is unsupported or
 * refuses the request.  Drag-and-drop cannot wait: handle_scene_view_asset_drop
 * must know the hovered entity inside the ImGui callback (Alt-replace vs
 * create-new is decided before the payload is released) and needs a fresh
 * hover answer on every frame of the drag; compute_surface_hit additionally
 * needs the ray parameter — a world-space hit POINT that an object-id buffer
 * does not carry at all.  Keep the limitation in mind before "fixing" a drop
 * that lands on the wrong object: the answer is finer bounds here, not the
 * GPU pick.
 *
 * `out_t` (optional) receives the ray parameter of the nearest hit; only
 * written when something was hit.  Returns the entity id, 0 = none. */
static uint32_t cpu_pick_entity_along_ray(const float ray_o[3],
                                          const float ray_d[3],
                                          float *out_t)
{
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

        float hx = fabsf(t->scale.x) * 0.5f;
        float hy = fabsf(t->scale.y) * 0.5f;
        float hz = fabsf(t->scale.z) * 0.5f;
        if (hx < 0.1f) hx = 0.1f;
        if (hy < 0.1f) hy = 0.1f;
        if (hz < 0.1f) hz = 0.1f;

        float t_hit;
        jce_vec3 ro    = jce_v3(ray_o[0], ray_o[1], ray_o[2]);
        jce_vec3 rd    = jce_v3(ray_d[0], ray_d[1], ray_d[2]);
        jce_vec3 bminv = jce_v3(t->position.x - hx, t->position.y - hy,
                                t->position.z - hz);
        jce_vec3 bmaxv = jce_v3(t->position.x + hx, t->position.y + hy,
                                t->position.z + hz);
        if (jce_ray_aabb_intersect(ro, rd, bminv, bmaxv, &t_hit)
            && t_hit >= 0.0f && t_hit < best_t) {
            best_t  = t_hit;
            best_id = pid;
        }
    }

    if (best_id != 0 && out_t) *out_t = best_t;
    return best_id;
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

    return cpu_pick_entity_along_ray(ray_o, ray_d, NULL);
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

/* Raycast the camera ray against scene-entity AABBs and return the nearest
 * world-space hit point — the real surface under the cursor — so a dropped model
 * lands ON whatever is beneath the cursor (Unity-style) instead of always on
 * Y=0. Falls back to the Y=0 ground plane when nothing is hit.
 *
 * Uses the coarse CPU picker, and could not use the GPU id pick even if it
 * were synchronous: what is wanted here is the HIT POINT along the ray, and
 * an object-id buffer stores ids, not depth. */
static bool compute_surface_hit(ImVec2 screen_pos, ImVec2 avail, float out_pos[3])
{
    float view_mat[16], proj_mat[16], eye[3];
    if (!jce_editor_scene_get_camera_matrices(view_mat, proj_mat, eye,
                                              avail.x, avail.y))
        return compute_ground_hit(screen_pos, avail, out_pos);

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

    float best_t = 0.0f;
    if (cpu_pick_entity_along_ray(ray_o, ray_d, &best_t) != 0) {
        out_pos[0] = ray_o[0] + ray_d[0] * best_t;
        out_pos[1] = ray_o[1] + ray_d[1] * best_t;
        out_pos[2] = ray_o[2] + ray_d[2] * best_t;
        return true;
    }
    return compute_ground_hit(screen_pos, avail, out_pos);
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
        float lo[3];
        float hi[3];
        if (!jce_editor_scene_camera_get_entity_focus_bounds(id, lo, hi))
            return;
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
            /* Standard-engine "Frame Selected" (Unity/Unreal/Blender): union the
             * selected entities' bounds, but EXCLUDE terrain when any non-terrain
             * entity is also selected — a terrain/ground entity's huge bounds
             * would otherwise dominate the union and fling the camera far out
             * (the reported "terrain + a house selected → focus is useless"
             * case). If ONLY terrain is selected, it is framed normally. */
            bool any_non_terrain = false;
            for (int i = 0; i < sel_n; i++) {
                if (sel[i] != 0 && jce_state_entity_exists(sel[i]) &&
                    !jce_scene_has_terrain(scene, (JceEntity)sel[i])) {
                    any_non_terrain = true;
                    break;
                }
            }
            for (int i = 0; i < sel_n; i++) {
                if (sel[i] == 0 || !jce_state_entity_exists(sel[i])) continue;
                if (any_non_terrain &&
                    jce_scene_has_terrain(scene, (JceEntity)sel[i])) continue;
                accumulate(sel[i]);
            }
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
            ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH,
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
            /* Default = create a new entity on the surface under the cursor;
             * hold Alt to instead REPLACE the hovered mesh entity (preview
             * switches to the entity hover-highlight only while Alt is down).
             * The ghost uses the RELATIVE path — the same cache key the created
             * entity will use — so the editor mesh cache is warmed during hover
             * and the drop can bottom-align against the real mesh AABB. */
            bool alt = ImGui::GetIO().KeyAlt;
            uint32_t hit_id = pick_entity_at_mouse(screen_pos, avail);
            if (alt && entity_accepts_mesh_material_drop(hit_id)) {
                jce_editor_scene_clear_ghost();
                jce_editor_scene_set_hover_entity(hit_id);
            } else {
                jce_editor_scene_clear_hover_entity();
                float hit[3];
                if (compute_surface_hit(screen_pos, avail, hit)) {
                    char rel_ghost[1024];
                    jce_editor_path_to_relative(rel_ghost, sizeof(rel_ghost),
                                                 asset_path);
                    const char *ghost_path = rel_ghost[0] ? rel_ghost : asset_path;
                    jce_editor_scene_set_ghost(ghost_path, hit[0], hit[1], hit[2]);
                }
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
            ImGui::AcceptDragDropPayload(JCE_DND_ASSET_PATH)) {
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
        /* ── Mesh dropped: default = create new; Alt = replace hovered mesh ─ */
        else if (is_mesh_asset(asset_path)) {
            /* Unity-style: a model drop ALWAYS creates a new entity. Replacing
             * an existing entity's mesh is opt-in via Alt, so dragging a model
             * onto a big ground plane no longer "attaches"/overwrites it. */
            uint32_t hit_id = pick_entity_at_mouse(screen_pos, avail);
            JceMeshRenderer *mesh_renderer_comp =
                ImGui::GetIO().KeyAlt ? find_mesh_renderer_component(hit_id) : NULL;
            if (mesh_renderer_comp) {
                /* Replace the existing entity's mesh + extract material. */
                char glb_buf[1024];
                const char *src_mesh = import_ensure_glb(asset_path, glb_buf,
                                                         sizeof(glb_buf));
                char rel_mesh[1024];
                jce_editor_path_to_relative(rel_mesh, sizeof(rel_mesh),
                                             src_mesh);
                const char *store_mesh = rel_mesh[0] ? rel_mesh : src_mesh;
                jce_state_begin_batch_edit();
                {
                    auto &mr = *mesh_renderer_comp;
                    mr.mesh_path = jce_scene_intern(jce_state_get_scene(), store_mesh);
                    mr.mesh_shape = 0;
                    mr.material_path = jce_scene_intern(jce_state_get_scene(), "");
                    mr.albedo_tex = jce_scene_intern(jce_state_get_scene(), "");
                    mr.mr_tex = jce_scene_intern(jce_state_get_scene(), "");
                    mr.normal_tex = jce_scene_intern(jce_state_get_scene(), "");
                    mr.ao_tex = jce_scene_intern(jce_state_get_scene(), "");
                    mr.emissive_tex = jce_scene_intern(jce_state_get_scene(), "");
                }
                jce_state_end_batch_edit();

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
                /* ── No Alt (or empty space): create a NEW entity on the real
                 *    surface under the cursor (Unity-style). ─ */
                float drop_pos[3] = { 0.0f, 0.0f, 0.0f };
                compute_surface_hit(screen_pos, avail, drop_pos);

                char name_buf[128];
                const char *fname = jce_editor_path_basename_view(asset_path);
                snprintf(name_buf, sizeof(name_buf), "%s", fname);
                jce_editor_path_strip_extension(name_buf);

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

                jce_state_begin_batch_edit();
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
                            char glb_buf2[1024];
                            const char *src_mesh2 = import_ensure_glb(asset_path,
                                                        glb_buf2, sizeof(glb_buf2));
                            char rel_mesh2[1024];
                            jce_editor_path_to_relative(rel_mesh2, sizeof(rel_mesh2),
                                                         src_mesh2);
                            const char *store_mesh2 = rel_mesh2[0]
                                                       ? rel_mesh2 : src_mesh2;
                            mr->mesh_path = jce_scene_intern(jce_state_get_scene(), store_mesh2);
                            mr->mesh_shape = 0;
                            mr->material_path = jce_scene_intern(jce_state_get_scene(), "");
                            mr->albedo_tex = jce_scene_intern(jce_state_get_scene(), "");
                            mr->mr_tex = jce_scene_intern(jce_state_get_scene(), "");
                            mr->normal_tex = jce_scene_intern(jce_state_get_scene(), "");
                            mr->ao_tex = jce_scene_intern(jce_state_get_scene(), "");
                            mr->emissive_tex = jce_scene_intern(jce_state_get_scene(), "");
                            jce_editor_console_log_level(JCE_CONSOLE_INFO,
                                "[drop-diag] new entity mesh_path='%s' exists=%d",
                                mr->mesh_path, (int)jce_fs_host_exists_file(mr->mesh_path));

                            /* A skinned/animated glTF would be stripped to a
                             * static bind pose by the Mesh Renderer (Assimp +
                             * PreTransformVertices drops skin/anim).  Detect a
                             * rig with a HEADER-ONLY cgltf probe — never a full
                             * model load — so dropping a heavy character (lots
                             * of clips) doesn't block the main thread building
                             * geometry + a clip player just to answer "skinned?".
                             * If it carries a skin and clips, attach a Skeletal
                             * Animator (the path that preserves skin+anim) and
                             * start playback; the actual model loads async via
                             * the normal render path. */
                            bool has_skin = false, has_anim = false;
                            jce_editor_probe_model_rig(store_mesh2,
                                                       &has_skin, &has_anim);
                            if (has_skin && has_anim) {
                                jce_state_add_component(
                                    id, JCE_COMP_FLAG_SKELETAL_ANIMATOR);
                                JceSkeletalAnimatorComponent *sa =
                                    jce_scene_get_skeletal_animator(
                                        scene, (JceEntity)id);
                                if (sa) {
                                    snprintf(sa->skeleton_path,
                                             sizeof(sa->skeleton_path),
                                             "%s", store_mesh2);
                                    sa->active_clip = 0;
                                    sa->loop        = true;
                                    sa->playing     = true;
                                    jce_editor_console_log_level(
                                        JCE_CONSOLE_INFO,
                                        "Skinned glTF: auto-added Skeletal "
                                        "Animator, playing");
                                }
                            }
                        }
                    }

                    /* Bottom-align: lift the new entity so its mesh AABB rests
                     * ON the surface (no half-sinking for centre-origin models).
                     * The hover ghost warmed the editor mesh cache under the
                     * same relative path, so the focus bounds here reflect the
                     * real mesh AABB; if the mesh is not cached yet this falls
                     * back to the unit-cube bounds (a small, harmless lift). */
                    if (scene) {
                        float fb_lo[3], fb_hi[3];
                        if (jce_editor_scene_camera_get_entity_focus_bounds(
                                id, fb_lo, fb_hi)) {
                            JceTransform *t2 =
                                jce_scene_get_transform(scene, (JceEntity)id);
                            if (t2)
                                t2->position.y += (drop_pos[1] - fb_lo[1]);
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
                jce_state_end_batch_edit();
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
    if (jce_scene_view_should_open_context_menu(ctx->viewport_right_clicked,
                                                ImGui::GetIO().KeyAlt))
        ImGui::OpenPopup("SceneViewContextMenu");

    if (ImGui::BeginPopup("SceneViewContextMenu")) {
        int sel_count = 0;
        jce_state_get_selection(&sel_count);
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

            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.duplicate"), "Ctrl+D"))
                jce_panel_duplicate_selection();

            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.delete"), "Delete"))
                jce_panel_delete_selection();

            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("scene.focusSelected"), "F")) {
                scene_view_frame_entities(false);
            }

            if (ImGui::BeginMenu(jce_editor_i18n("sceneView.gizmoMode"))) {
                JceGizmoMode menu_gm = jce_state_get_gizmo_mode();
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.translate"), "W", menu_gm == JCE_GIZMO_TRANSLATE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.rotate"), "E", menu_gm == JCE_GIZMO_ROTATE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.scale"), "R", menu_gm == JCE_GIZMO_SCALE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
                bool pivot_edit = jce_state_get_pivot_edit_mode();
                if (ImGui::MenuItem(jce_editor_i18n("sceneView.pivotEdit"), "D", pivot_edit))
                    jce_state_set_pivot_edit_mode(!pivot_edit);
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
            ImGui::Separator();
            const uint32_t focused = jce_state_get_focused();
            JceScene *scene = jce_state_get_scene();
            const bool has_camera = scene && focused != 0
                && jce_scene_get_camera(scene, (JceEntity)focused) != nullptr;
            char align_chord[64];
            char pilot_chord[64];
            jce_hotkey_chord_label(
                jce_hotkey_get(JCE_HK_VIEW_ALIGN_SELECTED_CAMERA),
                align_chord, sizeof(align_chord));
            jce_hotkey_chord_label(
                jce_hotkey_get(JCE_HK_VIEW_PILOT_SELECTED_CAMERA),
                pilot_chord, sizeof(pilot_chord));
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.camera.align"),
                                align_chord, false, has_camera))
                (void)jce_editor_scene_camera_align_selected();
            const char *pilot_label = jce_editor_scene_camera_is_piloting()
                ? jce_editor_i18n("sceneView.camera.stopPiloting")
                : jce_editor_i18n("sceneView.camera.pilot");
            if (ImGui::MenuItem(pilot_label, pilot_chord,
                                jce_editor_scene_camera_is_piloting(),
                                has_camera))
                (void)jce_editor_scene_camera_toggle_pilot();
            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }
}

/* ── Maya-style camera controls ──────────────────────────────────── */

static bool handle_scene_camera_controls(bool viewport_hovered)
{
    ImGuiIO &io = ImGui::GetIO();
    bool alt_held = io.KeyAlt;
    /* Default: wheel up -> zoom in. scene_camera_zoom(positive) brings
       the camera closer, MouseWheel is positive on wheel-up, so the
       default sign is +1. The pref opts INTO Apple natural-scroll. */
    float dy_sign     = jce_editor_pref_invert_drag_y      ? -1.0f : 1.0f;
    float wheel_sign  = jce_editor_pref_invert_scroll_zoom ? -1.0f : 1.0f;
    bool navigated = false;

    if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        float dyaw   = io.MouseDelta.x * 0.005f;
        float dpitch = io.MouseDelta.y * 0.005f * dy_sign;
        jce_editor_scene_camera_orbit(dyaw, dpitch);
        navigated = true;
    }

    /* Pan: "grab the world" — drag right → world slides right under cursor
       (camera moves left). MouseDelta.x positive when dragging right;
       jce_editor_scene_camera_pan already moves the camera by (-dx, -dy)
       internally, so passing raw MouseDelta yields the grab semantic. */
    if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)) {
        float pan_x = io.MouseDelta.x;
        float pan_y = io.MouseDelta.y * dy_sign;
        jce_editor_scene_camera_pan(pan_x, pan_y);
        navigated = true;
    }

    if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f)) {
        float zoom_delta = -io.MouseDelta.y * 0.05f * dy_sign;
        jce_editor_scene_camera_zoom(zoom_delta);
        navigated = true;
    }

    if (viewport_hovered && fabsf(io.MouseWheel) > 0.0f) {
        jce_editor_scene_camera_zoom(io.MouseWheel * wheel_sign);
        navigated = true;
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
        navigated = true;
    }
    return navigated;
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
        if (jce_hotkey_pressed(JCE_HK_GIZMO_PIVOT_EDIT)
            || ImGui::IsKeyPressed(ImGuiKey_Insert, false)) {
            jce_state_set_pivot_edit_mode(!jce_state_get_pivot_edit_mode());
        }

        const bool frame_sel  = jce_hotkey_pressed(JCE_HK_VIEW_FRAME_SELECTED);
        const bool frame_all_ = jce_hotkey_pressed(JCE_HK_VIEW_FRAME_ALL);
        if (frame_sel || frame_all_) {
            scene_view_frame_entities(frame_all_);
        }

        if (jce_hotkey_pressed(JCE_HK_VIEW_ALIGN_SELECTED_CAMERA))
            (void)jce_editor_scene_camera_align_selected();
        if (jce_hotkey_pressed(JCE_HK_VIEW_PILOT_SELECTED_CAMERA))
            (void)jce_editor_scene_camera_toggle_pilot();

        if (jce_hotkey_pressed(JCE_HK_EDIT_SNAP_TO_GROUND))
            jce_scene_view_snap_selection_to_ground();

        if (jce_hotkey_pressed(JCE_HK_EDIT_DELETE)
            || jce_hotkey_pressed(JCE_HK_EDIT_DELETE_ALT)) {
            jce_panel_delete_selection();
        }

        if (jce_hotkey_pressed(JCE_HK_EDIT_DUPLICATE)) {
            jce_panel_duplicate_selection();
        }
    }
}

/* ── Overlays + picking helpers ──────────────────────────────────── */

/* Marquee (box) selection goes through the GPU pick buffer: render the object-ID
 * buffer and read back the WHOLE drag rectangle, then select every unique entity
 * in it.  Pixel-accurate for any geometry — including streamed glTF chunk
 * objects — exactly like single-click picking.  (The CPU transform-scale AABB
 * test this replaced was wrong for glTF models, whose real world bounds are
 * scale × mesh-AABB, not a unit cube, so streamed objects couldn't be boxed.)
 * Async: requested on drag-finish here, polled on later frames in
 * handle_ray_pick. */
static bool s_gpu_marquee_pending = false;
static bool s_gpu_marquee_add     = false;

static void handle_marquee_selection(const SceneViewCtx *ctx,
                                     const float *view_mat,
                                     const float *proj_mat)
{
    (void)view_mat; (void)proj_mat;
    if (!s_sel_pending) return;
    s_sel_pending = false;
    if (!ctx || !jce_editor_scene_pick_supported()) return;

    /* Map the screen-space marquee rect to pick-buffer pixels (same mapping as
     * the single-click path in request_gpu_pick_for_click). */
    const uint32_t rt_w = (uint32_t)fmaxf(16.0f, floorf(ctx->avail.x));
    const uint32_t rt_h = (uint32_t)fmaxf(16.0f, floorf(ctx->avail.y));
    const float ax = ctx->avail.x > 1.0f ? ctx->avail.x : 1.0f;
    const float ay = ctx->avail.y > 1.0f ? ctx->avail.y : 1.0f;
    auto to_px = [&](float sx, float sy, uint32_t &ox, uint32_t &oy) {
        float lx = sx - ctx->screen_pos.x, ly = sy - ctx->screen_pos.y;
        if (lx < 0.0f) lx = 0.0f;
        if (ly < 0.0f) ly = 0.0f;
        if (lx > ax - 1.0f) lx = ax - 1.0f;
        if (ly > ay - 1.0f) ly = ay - 1.0f;
        uint32_t px = (uint32_t)floorf(lx * (float)rt_w / ax);
        uint32_t py = (uint32_t)floorf(ly * (float)rt_h / ay);
        if (px >= rt_w) px = rt_w - 1u;
        if (py >= rt_h) py = rt_h - 1u;
        if (jce_renderer_origin_bottom_left()) py = rt_h - 1u - py;
        ox = px; oy = py;
    };
    uint32_t x0, y0, x1, y1;
    to_px(s_sel_rect_min.x, s_sel_rect_min.y, x0, y0);
    to_px(s_sel_rect_max.x, s_sel_rect_max.y, x1, y1);

    if (jce_editor_scene_pick_request_rect(x0, y0, x1, y1)) {
        s_gpu_marquee_pending = true;
        s_gpu_marquee_add = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
    }
}

static bool s_gpu_pick_selection_pending = false;
static bool s_gpu_pick_add_mode = false;

static void cancel_deferred_scene_pick(void)
{
    s_sel_pending = false;
    s_sel_click_pending = false;
    s_gpu_pick_selection_pending = false;
    s_gpu_marquee_pending = false;
}

static void apply_single_pick_selection(uint32_t best_id, bool add_mode)
{
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

static bool request_gpu_pick_for_click(const SceneViewCtx *ctx,
                                       ImVec2 click_pos,
                                       bool add_mode)
{
    if (!ctx || !jce_editor_scene_pick_supported())
        return false;

    float local_x = click_pos.x - ctx->screen_pos.x;
    float local_y = click_pos.y - ctx->screen_pos.y;
    if (local_x < 0.0f || local_y < 0.0f ||
        local_x >= ctx->avail.x || local_y >= ctx->avail.y)
        return false;

    uint32_t rt_w = (uint32_t)fmaxf(16.0f, floorf(ctx->avail.x));
    uint32_t rt_h = (uint32_t)fmaxf(16.0f, floorf(ctx->avail.y));
    uint32_t px = (uint32_t)floorf(local_x * (float)rt_w / ctx->avail.x);
    uint32_t py = (uint32_t)floorf(local_y * (float)rt_h / ctx->avail.y);
    if (px >= rt_w) px = rt_w - 1u;
    if (py >= rt_h) py = rt_h - 1u;

    if (jce_renderer_origin_bottom_left())
        py = rt_h - 1u - py;

    if (!jce_editor_scene_pick_request(px, py))
        return false;

    s_gpu_pick_selection_pending = true;
    s_gpu_pick_add_mode = add_mode;
    return true;
}

/* Single-click ray pick. */
static void handle_ray_pick(const SceneViewCtx *ctx,
                            const float *view_mat,
                            const float *proj_mat,
                            const float *eye)
{
    /* Poll a pending GPU marquee (box) selection — fills with every unique
     * entity inside the dragged rectangle (streamed objects included). */
    if (s_gpu_marquee_pending) {
        static uint32_t marq_ids[4096];
        uint32_t mn = 0;
        if (jce_editor_scene_pick_poll_rect(marq_ids, 4096u, &mn)) {
            if (!s_gpu_marquee_add) jce_state_clear_selection();
            for (uint32_t i = 0; i < mn; i++)
                if (marq_ids[i]) jce_state_select_entity(marq_ids[i], true);

            /* Union in the screen-space helper icons (camera/light entities
             * have no id-buffer footprint).  Done at POLL time, after the
             * conditional clear above — selecting them at request time would
             * be wiped one frame later.  s_sel_rect_min/max persist. */
            uint32_t icon_total = 0;
            if (jce_state_show_flag(JCE_SHOW_FLAG_LIGHT_ICONS) ||
                jce_state_show_flag(JCE_SHOW_FLAG_CAMERA_ICONS) ||
                jce_state_show_flag(JCE_SHOW_FLAG_PARTICLE_ICONS)) {
                JceGizmoCamera marq_cam;
                memcpy(marq_cam.view, view_mat, sizeof(float) * 16);
                memcpy(marq_cam.proj, proj_mat, sizeof(float) * 16);
                memcpy(marq_cam.eye,  eye,      sizeof(float) * 3);
                marq_cam.viewport_size[0]   = ctx->avail.x;
                marq_cam.viewport_size[1]   = ctx->avail.y;
                marq_cam.viewport_origin[0] = ctx->screen_pos.x;
                marq_cam.viewport_origin[1] = ctx->screen_pos.y;
                uint32_t icon_ids[256];
                int icn = scene_helper_icons_in_rect(&marq_cam,
                                                     s_sel_rect_min, s_sel_rect_max,
                                                     icon_ids, 256);
                for (int ii = 0; ii < icn; ii++)
                    jce_state_select_entity(icon_ids[ii], true);
                icon_total = (uint32_t)icn;
            }

            jce_editor_inspector_request_sync();
            if (mn + icon_total > 0) jce_editor_layout_request_focus_inspector();
            s_gpu_marquee_pending = false;
        }
    }

    if (s_gpu_pick_selection_pending) {
        uint32_t gpu_hit = 0;
        if (jce_editor_scene_pick_poll(&gpu_hit)) {
            apply_single_pick_selection(gpu_hit, s_gpu_pick_add_mode);
            s_gpu_pick_selection_pending = false;
        } else {
            if (s_sel_click_pending)
                s_sel_click_pending = false;
            return;
        }
    }

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

    /* Screen-space icon pick FIRST: camera/light icons draw ON TOP of the
     * scene and never reach the GPU id-buffer — before this, clicking an
     * icon fell through to the id readback, decoded 0, and CLEARED the
     * selection.  Same visibility gate as the icon draw call. */
    if (jce_state_show_flag(JCE_SHOW_FLAG_LIGHT_ICONS) ||
        jce_state_show_flag(JCE_SHOW_FLAG_CAMERA_ICONS) ||
        jce_state_show_flag(JCE_SHOW_FLAG_PARTICLE_ICONS)) {
        uint32_t icon_hit = scene_helper_icon_hit_test(&pick_cam, s_sel_click_pos);
        if (icon_hit) {
            apply_single_pick_selection(icon_hit, add_mode);
            return;
        }
    }

    if (request_gpu_pick_for_click(ctx, s_sel_click_pos, add_mode))
        return;

    /* Coarse CPU fallback — only reached when the GPU id pick is unsupported
     * on this backend or refused the request (see request_gpu_pick_for_click);
     * every other click above returned already.  Same transform-scale AABB
     * test the drag-drop path uses, so the two never disagree about a hit
     * when selection lands here. */
    float ray_o[3], ray_d[3];
    gm_screen_to_ray(&pick_cam, s_sel_click_pos.x,
                     s_sel_click_pos.y, ray_o, ray_d);

    apply_single_pick_selection(cpu_pick_entity_along_ray(ray_o, ray_d, NULL),
                                add_mode);
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
        jce_state_show_flag(JCE_SHOW_FLAG_CAMERA_ICONS) ||
        jce_state_show_flag(JCE_SHOW_FLAG_PARTICLE_ICONS))
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

    /* Headless click-pick hook (JCE_DBG_PICK="<entity>@<frame>"): projects
     * the named entity's position to viewport pixels and injects it as a
     * REAL click (s_sel_click_*), so the full selection path runs — icon
     * hit-test, GPU id-buffer pick, poll, selection apply.  Result is
     * logged a second later.  Pairs with JCE_WINCAP_* for visual QA. */
    {
        static int  s_pk_frame = -2;   /* -2 unparsed, -1 disabled/fired */
        static char s_pk_name[64];
        static uint32_t s_pk_tick = 0;
        static int  s_pk_report = -1;  /* frame to report the outcome */
        static uint32_t s_pk_target = 0;
        ++s_pk_tick;
        if (s_pk_frame == -2) {
            s_pk_frame = -1;
            const char *v = getenv("JCE_DBG_PICK");
            const char *at = v ? strrchr(v, '@') : NULL;
            if (at && at > v) {
                size_t n = (size_t)(at - v);
                if (n >= sizeof(s_pk_name)) n = sizeof(s_pk_name) - 1;
                memcpy(s_pk_name, v, n);
                s_pk_name[n] = '\0';
                s_pk_frame = atoi(at + 1);
            }
        }
        if (s_pk_frame >= 0 && s_pk_tick >= (uint32_t)s_pk_frame) {
            s_pk_frame = -1;
            JceScene *scene = jce_state_get_scene();
            int count = jce_state_get_entity_count();
            for (int i = 0; i < count && scene; i++) {
                uint32_t id = jce_state_get_entity_id_by_index(i);
                const char *nm = jce_state_entity_name(id);
                if (!nm || strcmp(nm, s_pk_name) != 0) continue;
                JceTransform *tf = jce_scene_get_transform(scene, (JceEntity)id);
                if (!tf) break;
                JceGizmoCamera cam;
                memcpy(cam.view, view_mat, sizeof(float) * 16);
                memcpy(cam.proj, proj_mat, sizeof(float) * 16);
                memcpy(cam.eye,  eye,      sizeof(float) * 3);
                cam.viewport_size[0]   = ctx->avail.x;
                cam.viewport_size[1]   = ctx->avail.y;
                cam.viewport_origin[0] = ctx->screen_pos.x;
                cam.viewport_origin[1] = ctx->screen_pos.y;
                float world[3] = { tf->position.x, tf->position.y, tf->position.z };
                float screen[2];
                if (gm_world_to_screen(&cam, world, screen)) {
                    s_sel_click_pos     = ImVec2(screen[0], screen[1]);
                    s_sel_click_pending = true;
                    s_pk_target = id;
                    s_pk_report = (int)s_pk_tick + 60;
                    LOG_INFO("scene_view", "[dbg-pick] click at %.0f,%.0f for '%s' (id=%u)",
                             screen[0], screen[1], s_pk_name, id);
                } else {
                    LOG_WARN("scene_view", "[dbg-pick] '%s' projects off-screen", s_pk_name);
                }
                break;
            }
        }
        if (s_pk_report > 0 && s_pk_tick >= (uint32_t)s_pk_report) {
            s_pk_report = -1;
            LOG_INFO("scene_view", "[dbg-pick] result: '%s' selected=%d",
                     s_pk_name, jce_state_is_selected(s_pk_target) ? 1 : 0);
        }
    }

    handle_marquee_selection(ctx, view_mat, proj_mat);
    handle_ray_pick(ctx, view_mat, proj_mat, eye);
    draw_selection_outlines(ctx, view_mat, proj_mat);
}

/* ── Content (embeddable in tabs) ────────────────────────────────── */

#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>

/* Rolling sub-phase sampler (mirrors panel_phase in jce_editor_layout.cpp). */
static inline void panel_phase_sv(const char *name, uint64_t *t)
{
    uint64_t now = jce_time_perf_counter();
    jce_perf_phase_add(name, jce_time_perf_to_ms(*t, now));
    *t = now;
}

void jce_editor_panel_scene_view_content(void)
{
    /* Rolling sub-phases: the Scene View panel owns ~8ms/frame of
     * NON-render work at 150k (ed_p_scene_view minus scene_render) —
     * these locate which stage. */
    uint64_t sv_t = jce_time_perf_counter();
    flush_async_drop_material_extracts();
    draw_scene_view_toolbar();
    panel_phase_sv("ed_sv_toolbar", &sv_t);

    SceneViewCtx ctx;
    if (!setup_scene_viewport(&ctx)) {
        panel_phase_sv("ed_sv_vp", &sv_t);
        return;
    }
    panel_phase_sv("ed_sv_vp", &sv_t);

    draw_scene_context_menu(&ctx);
    const bool camera_navigated =
        handle_scene_camera_controls(ctx.viewport_hovered);
    const float viewport_aspect = ctx.avail.y > 1.0f
        ? ctx.avail.x / ctx.avail.y : 1.0f;
    jce_editor_scene_camera_update_pilot(camera_navigated, viewport_aspect);
    panel_phase_sv("ed_sv_cam", &sv_t);

    if (jce_scene_view_should_cancel_deferred_pick(
            ImGui::IsMouseClicked(ImGuiMouseButton_Left),
            ctx.viewport_left_clicked,
            ctx.viewport_active))
        cancel_deferred_scene_pick();

    /* ── Terrain brush (Phase 2-B.2) ─────────────────────────────
     *  Active only when the Terrain panel arms it. Steals LMB from
     *  selection so a click/drag inside the viewport raycasts onto
     *  the active terrain and applies the current brush at the
     *  hit point. Plain LMB only — Alt-LMB still orbits the camera. */
    bool brush_consumed = false;
    if (jce_scene_view_left_input_belongs_to_viewport(
            ctx.viewport_left_clicked,
            ctx.viewport_active,
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) &&
        (jce_terrain_panel_brush_armed() || jce_foliage_brush_armed()) &&
        !ImGui::GetIO().KeyAlt &&
        !jce_gizmo_is_active())
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
                float dt = ImGui::GetIO().DeltaTime;
                if (jce_terrain_panel_brush_armed())
                    jce_terrain_panel_apply_brush_world(hit[0], hit[2], dt);
                else if (jce_foliage_brush_armed())
                    jce_foliage_brush_apply_world(hit[0], hit[2], dt);
                brush_consumed = true;
            }
        }
    }
    /* End the brush stroke when LMB is released so the next drag becomes a
     * fresh undo entry (no-op if no stroke is in progress). */
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        jce_terrain_panel_end_brush_stroke();
        jce_foliage_brush_end_stroke();
    }

    if (!brush_consumed)
        handle_scene_selection_box(&ctx);
    else
        (void)0;
    if (jce_state_show_flag(JCE_SHOW_FLAG_GIZMOS))
        update_and_draw_scene_gizmo(&ctx);

    if (s_gizmo_transaction_open && !jce_gizmo_is_active()) {
        jce_state_commit_transaction();
        s_gizmo_transaction_open = false;
    }

    handle_scene_view_shortcuts();
    panel_phase_sv("ed_sv_mid", &sv_t);
    draw_scene_overlays_and_pick(&ctx);
    panel_phase_sv("ed_sv_overlay", &sv_t);
}

/* ── Standalone wrapper ──────────────────────────────────────────── */

void jce_editor_panel_scene_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW);
    if (!*vis) {
        jce_editor_scene_camera_stop_pilot();
        return;
    }

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
                        if (ImGui::Button(jce_editor_i18n("sceneView.camera.align")))
                            (void)jce_editor_scene_camera_align_selected();
                        ImGui::SameLine();
                        const bool piloting =
                            jce_editor_scene_camera_is_piloting();
                        if (ImGui::Button(jce_editor_i18n(piloting
                                ? "sceneView.camera.stopPiloting"
                                : "sceneView.camera.pilot")))
                            (void)jce_editor_scene_camera_toggle_pilot();
                        if (piloting) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.12f, 1.0f),
                                "%s", jce_editor_i18n("sceneView.camera.piloting"));
                        }
                        ImGui::TextDisabled("%s",
                                    jce_editor_i18n("inspector.camera.preview.noTarget"));
                    }
                    ImGui::End();
                }
            }
        }
    }
}
