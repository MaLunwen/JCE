/*
 * jce_editor_scene_render.cpp  Editor 3D scene rendering (FBO pipeline).
 *
 * Phase B refactor: scene rendering is delegated to the engine
 * `JceSceneRenderer` (sky, shadows, entities, sprites, IBL, post-fx
 * params). This file owns the overlay pipeline (grid, selection, ghost,
 * hover, physics debug) and the editor's PostFX bloom/tonemap chain.
 */

#include <jce/os/core/jce_perf_phase.h>
#include "io/jce_editor_mesh_predecode.h"
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_filesystem.h>

#include "io/jce_editor_file_util.h"
#include "jce_scene_content_context.h"
#include "jce_scene_render_internal.h"
#include "jce_editor_viewport_common.h"   /* plumbing shared with the Game View */
#include "core/jce_assetdb.h"
#include "core/jce_editor_project.h"
#include "ui/jce_editor_panels.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>   /* getenv for JCE_STREAM_SYNC bench toggle (M2) */

extern "C" {
#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_taa.h>   /* TSR jitter (Halton) for temporal upscale */
#include <jce/os/core/jce_console.h>   /* r.upscaler live cvar (Off/RCAS/TSR) */
#include <jce/renderer/jce_impostor.h>   /* modal bake owns the frame */
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>   /* jce_renderer_request_screenshot_fbo */
#include <jce/renderer/jce_render_settings.h>
#include <jce/middleware/scene/jce_lod.h>
#include <jce/renderer/jce_volumetric_fog.h>
}

/* C++ header (std::string): it MUST NOT sit in the extern "C" block above.
 * It did for one build, and the linker error named an UNMANGLED symbol while
 * printing a "could potentially match" hint whose demangled text looked
 * identical to the definition -- the two names differ only in linkage. */
#include "core/jce_editor_effective_render_settings.h"
#include <jce/renderer/jce_particles.h>
#include <jce/middleware/scene/jce_scene_probe_capture.h>

/* ── State instance (shared via extern in internal header) ────────── */

SceneRenderState s_sr;

/* ── Helpers (shared via internal header) ─────────────────────────── */

/* When non-UINT16_MAX, scene_view_id() returns this instead of the bridge view.
 * Used to redirect editor overlays into the post-PostFX FBO so gizmos render
 * crisply on top of the tone-mapped scene. */
static uint16_t s_view_id_override = UINT16_MAX;

uint16_t scene_view_id(void)
{
    if (s_view_id_override != UINT16_MAX)
        return s_view_id_override;
    if (s_sr.bridge)
        return jce_offscreen_target_get_view_id(s_sr.bridge);
    return (uint16_t)JCE_VIEW_EDITOR_SCENE;
}

JceMesh *get_cached_mesh(const char *mesh_path, const float *world_pos)
{
    return jce_editor_scene_asset_cache_get_mesh(mesh_path, world_pos);
}

JceTexture get_cached_texture(const char *material_path,
                                     const char *mesh_path)
{
    return jce_editor_scene_asset_cache_get_texture(material_path, mesh_path);
}

static bool ed_texture_failed_cb(const char *material_path,
                                 const char *mesh_path,
                                 void       *ud)
{
    (void)ud;
    return jce_editor_scene_asset_cache_texture_failed(material_path, mesh_path);
}

/* Generic asset-relative path resolver used by the engine for files it
 * loads directly via jce_fs (e.g. terrain meta JSON / bin pair).  We try
 * the assetdb project root first, then a recursive walk-up from the
 * scene file directory.  Returns true if a readable file was located. */
static bool ed_resolve_path_cb(const char *in, char *out, int outsz,
                               void *ud)
{
    (void)ud;
    return jce_editor_resolve_asset_path(in, out, outsz);
}

bool jce_editor_resolve_asset_path(const char *in, char *out, int outsz)
{
    if (!in || !*in || !out || outsz <= 0) return false;

    /* If `in` already opens, accept it as-is. */
    if (jce_fs_host_exists_file(in)) {
        snprintf(out, (size_t)outsz, "%s", in);
        return true;
    }

    /* Try assetdb root + path. */
    const char *root = jce_assetdb_get_root();
    if (root && root[0]) {
        char cand[1024];
        snprintf(cand, sizeof(cand), "%s/%s", root, in);
        if (jce_fs_host_exists_file(cand)) {
            snprintf(out, (size_t)outsz, "%s", cand);
            return true;
        }
    }

    /* Try walking up from the current scene file. */
    const char *spath = jce_state_get_current_scene_path();
    if (spath && spath[0]) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", spath);
        jce_editor_path_trim_to_parent(dir);
        for (int level = 0; level < 5; ++level) {
            if (!dir[0]) break;
            char cand[1024];
            snprintf(cand, sizeof(cand), "%s/%s", dir, in);
            if (jce_fs_host_exists_file(cand)) {
                snprintf(out, (size_t)outsz, "%s", cand);
                return true;
            }
            jce_editor_path_trim_to_parent(dir);
        }
    }
    return false;
}

/* World-streaming FS base: the directory chunk fragment paths
 * ("scenes/chunks/cell_*.scene.json") resolve against — the source ASSET ROOT,
 * i.e. the current scene file's grandparent (<root>/scenes/x.scene.json ->
 * <root>), the SAME base meshPath uses.  SHARED by both the scene-view preview
 * streamer and the editor-Play streamer so chunks resolve IDENTICALLY in both
 * viewports (prevents scene-view vs game-view divergence — the project root,
 * which jce_editor_assets_get_project() may return, is one level too high).
 * Falls back to the followed project root if the scene path is unknown. */
bool jce_editor_streaming_fs_base(char *out, int out_size)
{
    if (!out || out_size <= 0) return false;
    out[0] = '\0';
    const char *sp = jce_state_get_current_scene_path();
    if (sp && sp[0]) {
        snprintf(out, (size_t)out_size, "%s", sp);
        jce_editor_path_trim_to_parent(out);   /* -> <root>/scenes      */
        jce_editor_path_trim_to_parent(out);   /* -> <root> (asset root) */
        if (out[0]) return true;
    }
    const char *proj = jce_editor_assets_get_project();
    if (proj && proj[0]) { snprintf(out, (size_t)out_size, "%s", proj); return true; }
    return false;
}

/* ── Asset cache callbacks for the engine scene renderer ──────────── */

static JceMesh *ed_load_mesh_cb(const char *path, void *ud)
{
    (void)ud;
    return jce_editor_scene_asset_cache_get_mesh(path, NULL);
}

static JceModel *ed_load_model_cb(const char *path, void *ud)
{
    (void)ud;
    if (!path || path[0] == '\0') return NULL;

    /* Resolve through the editor mesh path resolver: tries direct, then
     * scene_dir + path, then recursive project search by basename. This
     * lets scene JSON reference models like "models/PSX_BagMan.glb"
     * regardless of editor cwd. */
    char resolved[512];
    const char *load_path = path;
    if (jce_editor_scene_asset_cache_resolve_mesh_path(
            path, resolved, (int)sizeof(resolved))) {
        load_path = resolved;
    }

    /* The mesh validation pass has usually already decoded this exact file on
     * a worker thread; claiming its result turns a ~92 ms decode+upload into a
     * ~11 ms upload.  Keyed on the raw component path, which is what both
     * sides have without depending on their two resolvers agreeing.  A miss is
     * ordinary -- validation may not have reached this model yet -- and falls
     * through to the synchronous path below, which is what always happened. */
    if (JceModelCpu *pre = jce_editor_mesh_predecode_take(path)) {
        return jce_model_upload_gltf_cpu(pre);   /* consumes pre */
    }

    size_t fsize = 0;
    void *buf = ed_read_file(load_path, &fsize);
    if (!buf) return NULL;
    JceModel *m = jce_model_load_gltf_memory(buf, (uint32_t)fsize, load_path);
    ED_FREE(buf);
    return m;
}

bool jce_editor_probe_model_rig(const char *asset_path,
                                bool *out_has_skin, bool *out_has_anim)
{
    if (out_has_skin) *out_has_skin = false;
    if (out_has_anim) *out_has_anim = false;
    if (!asset_path || asset_path[0] == '\0') return false;

    char resolved[512];
    const char *load_path = asset_path;
    if (jce_editor_scene_asset_cache_resolve_mesh_path(
            asset_path, resolved, (int)sizeof(resolved))) {
        load_path = resolved;
    }

    /* Reading the bytes is pure (cached) I/O; the probe parses only the
     * glTF header — no geometry, buffers, images, or clip player.  GLB
     * needs the full file present (cgltf validates the declared length),
     * so we read it whole but never build the model. */
    size_t fsize = 0;
    void *buf = ed_read_file(load_path, &fsize);
    if (!buf) return false;
    bool ok = jce_model_probe_rig_memory(buf, (uint32_t)fsize,
                                         out_has_skin, out_has_anim);
    ED_FREE(buf);
    return ok;
}

static JceTexture ed_load_texture_cb(const char *material_path,
                                     const char *mesh_path,
                                     void       *ud)
{
    (void)ud;
    /* Editor uses its async asset cache, which understands material JSON,
     * OBJ MTL, basename fallback, and filesystem paths (not just PAK).
     * Passing both paths into a single asset_cache call collapses them
     * into ONE cache entry — matches 0.5.5 single-resolve behavior. */
    return jce_editor_scene_asset_cache_get_texture(material_path, mesh_path);
}

static JceTexture ed_load_texture_srgb_cb(const char *material_path,
                                          const char *mesh_path,
                                          void       *ud)
{
    (void)ud;
    return jce_editor_scene_asset_cache_get_texture_srgb(material_path,
                                                         mesh_path);
}

/* ── Animation query cache (forward state for shutdown) ───────────── */

#define ED_QUERY_CACHE_MAX 16
struct EdQueryCacheEntry {
    char           path[256];
    JceModel      *model;
    JceAnimPlayer *player;
    bool           used;
};
static EdQueryCacheEntry s_query_cache[ED_QUERY_CACHE_MAX];

static JceFileSystem *s_content_context_fs = nullptr;
static JceFsActivePolicy s_content_context_policy = JCE_FS_ACTIVE_OVERLAY;
static char s_content_context_project_root[1024] = {0};

/* Apply the complete content-addressing context before scene components begin
 * resolving assets.  Source scenes use project-root paths; Bundle Preview is
 * hermetic and reads project settings/components only through its isolated VFS.
 * Keeping grass and particle addressing together prevents the editor from
 * rendering a hybrid of the selected bundle and the previously open project. */
static void sr_apply_content_context(void)
{
    JceFileSystem *active_fs = jce_fs_get_active();
    JceFsActivePolicy active_policy = jce_fs_get_active_policy();
    const bool isolated = active_fs != nullptr &&
                          active_policy == JCE_FS_ACTIVE_ISOLATED;
    const JceProject *proj = jce_editor_project_get();
    const JceEditorSceneContentPaths paths = jce_editor_scene_content_paths(
        isolated,
        proj ? proj->project_root : nullptr,
        proj ? proj->source_assets : nullptr,
        proj ? proj->cooked_assets : nullptr);

    s_content_context_fs = active_fs;
    s_content_context_policy = active_policy;
    snprintf(s_content_context_project_root,
             sizeof(s_content_context_project_root), "%s",
             (proj && proj->project_root) ? proj->project_root : "");

    jce_scene_particles_set_asset_root(
        paths.particle_asset_root.empty()
            ? nullptr : paths.particle_asset_root.c_str());

    /* Canvas UI fonts follow the same project content root, so UIText
     * resolves project-authored fonts (e.g. space's SegoeUI.ttf) instead of
     * relying on a key collision with the editor's embedded pak. */
    jce_ui_canvas_set_asset_root(
        paths.particle_asset_root.empty()
            ? nullptr : paths.particle_asset_root.c_str());
    /* And the project's fallback face for an empty fontPath -- same reason as
     * the asset root: it was only reachable from a project's own main(), so
     * the editor could not know it and rendered such text in a different
     * typeface than the shipped exe. */
    jce_ui_canvas_set_default_font(proj ? proj->ui_default_font : nullptr);
    jce_ui_canvas_set_font_fallbacks(proj ? proj->ui_font_fallbacks
                                        : nullptr);

    if (!s_sr.scene_renderer) return;

    /* Same composition the packager uses -- authored file for the Look Profile
     * and the grass gate, active quality level on top -- so what Play shows is
     * what the build produces.  This used to read the file only, which is why
     * the quality level never reached the viewport. */
    JceRenderSettings rs = jce_render_settings_default();
    std::string settings_path;
    const bool loaded = jce_editor_effective_render_settings(
        isolated,
        (proj && proj->project_root) ? proj->project_root : nullptr,
        &rs, &settings_path);

    /* One line for every project-level render setting the editor applies, so
     * "does Play match the build?" is answerable from the log.  It used to
     * shout GRASS GATE and report only that one field. */
    jce_scene_renderer_set_grass_enabled(s_sr.scene_renderer, rs.grass_enabled != 0);
    /* Parity: the shipped drop-in main applies the same authored value.  The
     * editor applied NOTHING here, so the LOD bias a designer set was visible
     * only after a build -- and then as the wrong quantity entirely. */
    jce_lod_set_global_bias(rs.lod_bias);
    /* Soft particles, same parity argument: the hard seam where a billboard
     * cuts the floor is exactly what an author is looking at when they tick
     * the box, so the viewport has to show the fade rather than the build
     * being the first place it appears.  Editor Play applies the same value
     * from the same quality level, so the two cannot disagree. */
    jce_particles_set_soft_fade_distance(
        rs.soft_particles ? JCE_PARTICLES_SOFT_FADE_DEFAULT : 0.0f);

    /* Reported AFTER applying, and read back out of the engine rather than
     * echoed from the request: a bias of 0 or NaN is rejected and the engine
     * keeps 1.0, which is exactly the case a reader needs to be able to see.
     * (The first cut logged before the call and would have printed the
     * PREVIOUS value on every project change.) */
    LOG_INFO("scene_render",
             "project render settings %s from '%s' (root %s): grass=%d "
             "lod_bias=%.2f softparticles=%.2f",
             loaded ? "applied" : "NOT FOUND, using defaults",
             settings_path.c_str(),
             isolated ? "(bundle-vfs)" :
                 ((proj && proj->project_root) ? proj->project_root : "(none)"),
             (int)rs.grass_enabled,
             (double)jce_lod_get_global_bias(),
             (double)jce_particles_get_soft_fade_distance());
}

void jce_editor_scene_render_refresh_content_context(void)
{
    sr_apply_content_context();
}

/* ── Init ─────────────────────────────────────────────────────────── */

bool jce_editor_scene_render_init(JceRenderer *renderer,
                                  const JcePakArchive *pak,
                                  JceAssetManager *assets)
{
    if (s_sr.initialized) return true;

    memset(&s_sr, 0, sizeof(s_sr));
    jce_editor_scene_asset_cache_init(assets);
    s_sr.white_tex.idx = UINT16_MAX;
    s_sr.postfx_output_tex = UINT16_MAX;
    s_sr.renderer = renderer;
    /* ECS-UI Canvas overlay for the Scene View (mirrors the game view's
     * instance; draw-only — the Game View owns pointer interaction). */
    s_sr.ui_canvas = jce_ui_canvas_create(renderer, pak);
    if (!s_sr.ui_canvas)
        LOG_WARN(LOG_TAG, "failed to create scene-view UI canvas renderer");

    s_sr.bridge = jce_offscreen_target_create(renderer,
                                                  (uint16_t)JCE_VIEW_EDITOR_SCENE);
    if (!s_sr.bridge) {
        LOG_WARN(LOG_TAG, "failed to create editor render bridge");
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

    /* Native-resolution upscale target for the dynamic-resolution RCAS resolve.
     * Its own view (base+POST_BASE+23 = 46) sits after the postfx composite (+21)
     * and canvas UI (+22) and before the screenshot readback (+24), so the
     * resolve reads the fully composited bridge. Non-fatal: absence just falls
     * back to ImGui bilinear upscale. */
    s_sr.present = jce_offscreen_target_create(
        renderer, (uint16_t)(JCE_VIEW_EDITOR_SCENE +
                             JCE_EDITOR_VP_UPSCALE_OFFSET));
    s_sr.present_tex = UINT16_MAX;
    if (!s_sr.present)
        LOG_WARN(LOG_TAG, "failed to create editor upscale target (RCAS disabled)");

    JceGfxCaps caps = jce_gfx_caps();
    s_sr.homogeneous_depth = caps.homogeneous_depth;

    /* Create the editor orbit camera. */
    JceCameraDesc cam_desc;
    memset(&cam_desc, 0, sizeof(cam_desc));
    cam_desc.mode       = JCE_CAMERA_PERSPECTIVE;
    cam_desc.position   = jce_v3(8.0f, 6.0f, 8.0f);
    cam_desc.target     = jce_v3(0.0f, 0.0f, 0.0f);
    cam_desc.up         = jce_v3(0.0f, 1.0f, 0.0f);
    cam_desc.fov_deg    = 45.0f;
    cam_desc.near_plane = 0.1f;
    cam_desc.far_plane  = 500.0f;

    s_sr.camera = jce_camera_create(&cam_desc);
    if (!s_sr.camera) {
        LOG_WARN(LOG_TAG, "failed to create editor camera");
        if (s_sr.present) { jce_offscreen_target_destroy(s_sr.present); s_sr.present = NULL; }
        if (s_sr.bridge) {
            jce_offscreen_target_destroy(s_sr.bridge);
            s_sr.bridge = NULL;
        }
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

    /* Initialize orbit state from the camera's initial position/target. */
    s_sr.orbit_target = jce_v3(0.0f, 0.0f, 0.0f);
    jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
    jce_vec3 diff = jce_v3_sub(cam_pos, s_sr.orbit_target);
    s_sr.orbit_distance = jce_v3_len(diff);
    s_sr.orbit_yaw   = atan2f(diff.x, -diff.z);
    s_sr.orbit_pitch = asinf(diff.y / s_sr.orbit_distance);
    s_sr.orbit_clip_valid = false;
    s_sr.camera_cache_valid = false;

    /* Pos + color vertex layout for transient buffers (grid / overlays). */
    jce_vertex_layout_begin(&s_sr.layout);
    jce_vertex_layout_add(&s_sr.layout, JCE_ATTRIB_POSITION, 3,
                          JCE_ATTRIB_TYPE_FLOAT, false, false);
    jce_vertex_layout_add(&s_sr.layout, JCE_ATTRIB_COLOR0, 4,
                          JCE_ATTRIB_TYPE_UINT8, true, false);
    jce_vertex_layout_end(&s_sr.layout);

    /* Cache the color shader program handle. */
    JceShaderHandle sh = jce_renderer_get_program_color(renderer);
    s_sr.prog_color.idx = sh.idx;

    /* Load editor-only grid shader from the PAK archive. */
    JceShaderHandle grid_sh = shader_load_program(pak, "grid");
    s_sr.prog_grid.idx = grid_sh.idx;
    if (grid_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "grid shader not found in PAK — grid will be skipped");

    s_sr.u_grid_camera = jce_uniform_create("u_grid_camera",
                                            JCE_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_grid_fade = jce_uniform_create("u_grid_fade",
                                          JCE_UNIFORM_TYPE_VEC4, 1);

    /* Lighting uniforms for flat-color overlay (selection / ghost / hover). */
    s_sr.u_light_dir   = jce_uniform_create("u_lightDir",
                                            JCE_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_light_color = jce_uniform_create("u_lightColor",
                                            JCE_UNIFORM_TYPE_VEC4, 1);

    /* 1x1 white fallback texture for overlay binding. */
    {
        uint32_t white = 0xFFFFFFFF;
        const JceGfxMemory *mem = jce_gfx_memory_copy(&white, 4);
        s_sr.white_tex = jce_texture_create_2d(1, 1, false, 1,
                                               JCE_TEXTURE_FORMAT_RGBA8,
                                               JCE_TEXTURE_FLAGS_NONE, mem);
    }

    /* Create the engine scene renderer with editor asset callbacks. */
    JceSceneRendererCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.load_mesh    = ed_load_mesh_cb;
    cbs.load_model   = ed_load_model_cb;
    cbs.load_texture    = ed_load_texture_cb;
    cbs.load_texture_srgb = ed_load_texture_srgb_cb;
    cbs.texture_failed  = ed_texture_failed_cb;
    cbs.resolve_path    = ed_resolve_path_cb;
    cbs.userdata        = NULL;
    s_sr.scene_renderer = jce_scene_renderer_create(renderer, pak, &cbs);
    if (!s_sr.scene_renderer) {
        LOG_WARN(LOG_TAG, "failed to create engine scene renderer");
        if (s_sr.camera) { jce_camera_destroy(s_sr.camera); s_sr.camera = NULL; }
        if (s_sr.present) { jce_offscreen_target_destroy(s_sr.present); s_sr.present = NULL; }
        if (s_sr.bridge) { jce_offscreen_target_destroy(s_sr.bridge); s_sr.bridge = NULL; }
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

    /* Grass project gate (Stage 1b.6): applied from the open project's
     * render_settings.json (grassEnabled).  No project is open yet at editor
     * init (it is opened by a dialog later), so this also RE-APPLIES per-frame
     * on project change — see sr_apply_content_context() called from
     * jce_editor_scene_render_frame(). */
    sr_apply_content_context();

    JceScenePickDesc pick_desc;
    memset(&pick_desc, 0, sizeof(pick_desc));
    pick_desc.renderer  = renderer;
    pick_desc.pak       = pak;
    pick_desc.callbacks = &cbs;
    /* Pick borrows skinned models from the scene renderer's cache (no second
     * GPU copy); shutdown order (pick pass before scene renderer) upholds the
     * borrow contract documented in pick_resolve_model. */
    pick_desc.scene_renderer = s_sr.scene_renderer;
    pick_desc.view_id   = (uint16_t)JCE_VIEW_EDITOR_PICK;
    s_sr.pick_pass = jce_scene_pick_create(&pick_desc);
    if (!s_sr.pick_pass)
        LOG_WARN(LOG_TAG, "scene GPU picking unavailable");

    s_sr.anim_last_ticks = 0;

    /* Occlusion culler: GPU-query two-pass coherence culling.
     * Only the 'color' program is needed for the depth-only proxy draw.
     * The culler silently degrades to always-visible when hardware
     * occlusion queries are unsupported (ES2 / WebGL1).
     * Opt-IN via JCE_ENABLE_OCCLUSION=1 (A/B measurement) —
     * mirrors the game-view path so the documented toggle disables BOTH editor
     * viewports, not just Play. */
    if (!jce_editor_viewport_occlusion_enabled()) {
        s_sr.occlusion_culler = NULL;
        LOG_INFO(LOG_TAG,
            "%s occlusion culling OFF (%s)", "scene-view",
            jce_editor_viewport_occlusion_forced_by_env()
                ? "JCE_ENABLE_OCCLUSION=0"
                : "off by default; enable with Preferences > Viewport > occlusion culling, or JCE_ENABLE_OCCLUSION=1");
    } else {
        /* Default proxy view; the game view creates its own on a distinct one. */
        s_sr.occlusion_culler =
            jce_editor_viewport_create_occlusion_culler(renderer, -1);
        if (!s_sr.occlusion_culler)
            LOG_WARN(LOG_TAG, "occlusion culler creation failed (culling disabled)");
    }

    /* World streamer: built from the scene's authored streaming settings
       (World Streaming panel) — created only when the scene enables
       streaming AND the session preview toggle is on. */
    jce_editor_scene_render_streaming_rebuild();

    s_sr.initialized = true;
    LOG_INFO(LOG_TAG, "editor scene renderer initialized (engine-backed)");
    return true;
}

/* ── Shutdown ─────────────────────────────────────────────────────── */

void jce_editor_scene_render_shutdown(void)
{
    if (!s_sr.initialized) return;

    if (s_sr.ui_canvas) {
        jce_ui_canvas_destroy(s_sr.ui_canvas);
        s_sr.ui_canvas = NULL;
    }
    if (s_sr.pick_pass) {
        jce_scene_pick_destroy(s_sr.pick_pass);
        s_sr.pick_pass = NULL;
    }

    if (s_sr.scene_renderer) {
        jce_scene_renderer_destroy(s_sr.scene_renderer);
        s_sr.scene_renderer = NULL;
    }

    /* Free the editor-side animation query cache. */
    for (int i = 0; i < ED_QUERY_CACHE_MAX; ++i) {
        EdQueryCacheEntry &e = s_query_cache[i];
        if (!e.used) continue;
        if (e.player) { jce_anim_player_destroy(e.player); e.player = nullptr; }
        if (e.model)  { jce_model_destroy(e.model);        e.model  = nullptr; }
        e.used = false;
    }

    jce_editor_scene_asset_cache_shutdown();

    if (s_sr.present) {
        jce_offscreen_target_destroy(s_sr.present);
        s_sr.present = NULL;
    }

    if (s_sr.bridge) {
        jce_offscreen_target_destroy(s_sr.bridge);
        s_sr.bridge = NULL;
    }

    if (s_sr.camera)     { jce_camera_destroy(s_sr.camera);   s_sr.camera = NULL; }

    if (jce_gfx_texture_valid(s_sr.white_tex))
        jce_gfx_texture_destroy(s_sr.white_tex);

    if (jce_program_valid(s_sr.prog_grid))
        jce_program_destroy(s_sr.prog_grid);
    if (jce_uniform_valid(s_sr.u_grid_camera))
        jce_uniform_destroy(s_sr.u_grid_camera);
    if (jce_uniform_valid(s_sr.u_grid_fade))
        jce_uniform_destroy(s_sr.u_grid_fade);
    if (jce_uniform_valid(s_sr.u_light_dir))
        jce_uniform_destroy(s_sr.u_light_dir);
    if (jce_uniform_valid(s_sr.u_light_color))
        jce_uniform_destroy(s_sr.u_light_color);

    s_sr.initialized = false;

    if (s_sr.world_streamer) {
        jce_world_streamer_destroy(s_sr.world_streamer);
        s_sr.world_streamer = NULL;
    }
    if (s_sr.stream_fs) {
        jce_fs_destroy(s_sr.stream_fs);
        s_sr.stream_fs = NULL;
    }
    if (s_sr.occlusion_culler) {
        jce_occlusion_culler_destroy(s_sr.occlusion_culler);
        s_sr.occlusion_culler = NULL;
    }

    LOG_INFO(LOG_TAG, "editor scene renderer shutdown");
}

JceSceneRenderer *jce_editor_get_scene_renderer(void)
{
    return s_sr.scene_renderer;
}

JceRenderer *jce_editor_get_renderer(void)
{
    return s_sr.renderer;
}

JceWorldStreamer *jce_editor_get_world_streamer(void)
{
    return s_sr.world_streamer;
}

/* ── World-streaming preview lifecycle ────────────────────────────── */

void jce_editor_scene_render_streaming_teardown(void)
{
    /* Destroying the streamer also destroys every chunk entity it spawned
       into the live scene — this is the save-safety mechanism that keeps
       streamed content out of the serialized main scene. */
    if (s_sr.world_streamer) {
        jce_world_streamer_destroy(s_sr.world_streamer);
        s_sr.world_streamer = NULL;
        /* Re-show all HLOD proxies so the master skyline is whole again once
         * the preview streamer is gone (no chunk is resident to hide them). */
        jce_state_detach_streamer_hlod();
    }
    if (s_sr.stream_fs) {
        jce_fs_destroy(s_sr.stream_fs);
        s_sr.stream_fs = NULL;
    }
}

void jce_editor_scene_render_invalidate_model_caches(void)
{
    /* Scene-switch cache reset: the renderer's model cache never evicts and
     * caches load FAILURES, and the pick pass keeps its own model cache — both
     * survive a scene swap, so a model that failed (or a name that collided
     * with a missing asset) under the previous scene would never reload until
     * an editor restart.  Dropping them here makes switching scenes behave like
     * a fresh start.  The editor mesh/texture caches + resolve-miss cache are
     * cleared separately by the scene loader. */
    if (s_sr.scene_renderer)
        jce_scene_renderer_invalidate_model_cache(s_sr.scene_renderer);
    if (s_sr.pick_pass)
        jce_scene_pick_invalidate_model_cache(s_sr.pick_pass);
    /* The occlusion culler is keyed by entity id; the fresh world reuses
     * those ids, so stale slots hand recreated entities dead cull verdicts
     * and leak the hard-capped bgfx query pool. */
    jce_editor_scene_render_reset_occlusion();
}

void jce_editor_scene_render_reset_occlusion(void)
{
    if (s_sr.occlusion_culler)
        jce_occlusion_culler_reset(s_sr.occlusion_culler);
    /* The engine scene renderer keeps entity-keyed environment caches
     * (vegetation scatter / grass / water / foliage-cluster canopies).
     * The recreated ECS world hands recycled ids back with bumped
     * generation bits, stranding every slot; a full fcluster table then
     * silently skips drawing new clusters (leaves + grass vanishing after
     * undo).  Drop them on the same trigger as the occlusion slots —
     * content rebuilds lazily on the next draw. */
    if (s_sr.scene_renderer)
        jce_scene_renderer_reset_entity_caches(s_sr.scene_renderer);
}

void jce_editor_scene_render_streaming_rebuild(void)
{
    jce_editor_scene_render_streaming_teardown();

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    /* Session preview toggle (off by default): authoring the chunk table
       must not mutate the hierarchy until the user opts in. */
    if (!jce_state_get_streaming_preview()) return;

    const JceSceneStreamingSettings *st =
        jce_scene_get_streaming_settings(scene);
    if (!st || !st->enabled) return;

    JceFileSystem *fs = jce_fs_create();
    if (!fs) return;

    /* Mount the source ASSET ROOT so chunk fragment paths resolve the same as
       meshPath.  SHARED with the editor-Play streamer via
       jce_editor_streaming_fs_base() so the scene-view preview and the game-view
       Play stream byte-identical content (no viewport divergence). */
    char base[1024] = { 0 };
    if (!jce_editor_streaming_fs_base(base, sizeof(base))) {
        jce_fs_destroy(fs);
        LOG_WARN(LOG_TAG, "world-streaming preview: no asset root resolved");
        return;
    }
    if (!jce_fs_mount_dir(fs, "", base)) {
        jce_fs_destroy(fs);
        LOG_WARN(LOG_TAG, "world-streaming preview: cannot mount asset root '%s'",
                 base);
        return;
    }

    JceWorldStreamConfig wsc = jce_world_stream_config_default();
    wsc.mode            = (st->mode == 1) ? JCE_STREAM_RECTANGULAR
                                          : JCE_STREAM_RADIAL;
    wsc.load_radius     = st->load_radius;
    wsc.unload_radius   = st->unload_radius;
    wsc.max_pending     = st->max_pending;
    wsc.budget_mb       = st->budget_mb;
    wsc.frame_budget_ms = st->frame_budget_ms;

    /* WorldStreamer owns its bounded structured executor. The diagnostic
     * switch selects cooperative execution without allocating an idle pool. */
    {
        const char *ss = getenv("JCE_STREAM_SYNC");
        wsc.single_thread = ss && ss[0] && ss[0] != '0';
    }

    JceWorldStreamer *ws = jce_world_streamer_create(&wsc, scene, fs, NULL);
    if (!ws) {
        jce_fs_destroy(fs);
        LOG_WARN(LOG_TAG, "world streamer creation failed (preview disabled)");
        return;
    }
    jce_world_streamer_register_from_scene_settings(ws, st);

    s_sr.world_streamer = ws;
    s_sr.stream_fs      = fs;
    /* Mirror streamed chunk entities into the editor hierarchy/selection so
     * they are first-class (listed in the Hierarchy panel, selectable). */
    jce_state_attach_streamer_hierarchy(ws);
    /* Toggle the always-resident HLOD far-skyline proxies as chunks (un)load so
     * the scene-view far skyline isn't empty beyond the resident window. */
    jce_state_attach_streamer_hlod(ws);
    /* Re-apply the session preview mode + filter (Full-World / Filtered) to the
     * fresh streamer — each settings edit recreates it, so the user's intent
     * must be pushed back in or it would silently revert to RADIUS. */
    jce_state_streaming_apply_preview();
    LOG_INFO(LOG_TAG, "world-streaming preview active (%u chunks, root=%s)",
             jce_world_streamer_chunk_count(ws), base[0] ? base : ".");
}

/* On scene load: auto-enable the streaming preview for streaming-enabled scenes
 * so the EDITOR scene view shows the streamed world too — matching what Play
 * shows — instead of looking empty until you press Play.  The user can still
 * toggle it off via the World Streaming panel.  Then (re)build the streamer. */
void jce_editor_scene_render_streaming_autostart(void)
{
    JceScene *scene = jce_state_get_scene();
    if (scene) {
        const JceSceneStreamingSettings *st = jce_scene_get_streaming_settings(scene);
        if (st && st->enabled && st->chunk_count > 0)
            jce_state_set_streaming_preview(true);
    }
    jce_editor_scene_render_streaming_rebuild();
}

/* ── Per-frame ────────────────────────────────────────────────────── */

/* Last scene-view render dimensions, captured for headless self-capture
 * (jce_editor_scene_render_screenshot sizes its read-back staging to match). */
static uint16_t s_cap_w = 0, s_cap_h = 0;

/* Sub-attribution of the editor-side viewport frame: ed_sv_vp measured 9.87 ms
 * against the engine's scene_render at 6.77 ms, so ~3.1 ms/frame is spent here,
 * around the engine call.  ~10 marks per frame - free at this granularity. */
static inline void ed_vp_mark(const char *name, uint64_t *t)
{
    uint64_t now = jce_time_perf_counter();
    jce_perf_phase_add(name, jce_time_perf_to_ms(*t, now));
    *t = now;
}

void jce_editor_scene_render_frame(uint32_t width, uint32_t height)
{
    uint64_t _vpt = jce_time_perf_counter();
    if (!s_sr.initialized || !s_sr.renderer) return;
    if (width == 0 || height == 0) return;

    /* A modal impostor bake owns the frame's view ids (127..227), which is
     * inside the Game View's range, and the guard reports it either way.
     * Yield rather than share: the panel shows its last frame for the two or
     * three frames a bake takes.  That is what an Unreal HLOD build and a
     * Unity lightmap bake do with the live viewport, and the alternative is
     * the failure this whole band of work exists to remove -- two owners on
     * one view id and one of them silently producing nothing.
     *
     * The bake is driven from the Inspector / the QA harness, not from here,
     * so skipping this render does not stall it. */
    if (jce_impostor_bake_in_flight()) return;

    /* REFLECTION PROBE CAPTURE, driven here and yielded to here.
     *
     * Driven: this is the one place that runs every frame AND holds both the
     * scene renderer and the JceRenderer the capture needs, so a probe bake
     * proceeds whichever panel started it and whether or not that panel is
     * still open.
     *
     * Yielded to: the capture renders the scene from THIS viewport's base id,
     * because a full scene render claims 117 view ids and bgfx's 256 do not
     * hold a third span of that size.  Same arrangement, and same reason, as
     * the impostor bake immediately above. */
    if (jce_scene_probe_capture_in_flight()) {
        (void)jce_scene_probe_capture_poll(s_sr.scene_renderer,
                                           jce_state_get_scene(),
                                           jce_editor_get_renderer());
        return;
    }


    /* iGPU ADAPTIVE dynamic resolution (editor scene view): steer the 3D render
     * scale from the LAST frame's measured GPU time (Unity DynamicResolution /
     * UE dynamic-res).  A light scene rides the scale back to 1.0 (crisp); only a
     * genuinely GPU-bound frame downscales, at ANY panel size.  The smaller color
     * target is upscaled by the panel's ImGui::Image (uv=1.0, target sized exactly
     * to the render); disp_w/disp_h keep the NATIVE size for the on-demand full-
     * res PICK pass, so click->entity stays pixel-exact regardless of scale (pick
     * coords are avail-space).  width/height below become the reduced RENDER size
     * (color target + cfg viewport; camera aspect stays proportional).
     *
     * FLICKER GUARD (this is an editing surface): a WIDE dead zone [0.70*target,
     * target] plus small ASYMMETRIC steps — shrink quickly to relieve a hitch,
     * grow slowly — so the viewport settles instead of hunting while orbiting.
     * Gizmos/grid composite into the color target and soften when scaled (accepted
     * for FPS).  iGPU MEDIUM only; discrete GPUs render 1:1.  JCE_DYNRES=0 forces
     * native; JCE_DYNRES_SCALE=<0.25..1> pins a fixed scale (QA / A-B). */
    const uint32_t disp_w = width, disp_h = height;
    if (jce_renderer_get_tier() == JCE_GPU_TIER_MEDIUM &&
        !jce_renderer_get_recommendation().has_discrete_gpu) {
        static int   s_mode  = -1;    /* -1 unresolved, 0 off, 1 adaptive, 2 fixed */
        static float s_fixed = 1.0f;
        if (s_mode < 0) {
            s_mode = 1;
            if (const char *d = getenv("JCE_DYNRES")) { if (d[0] == '0') s_mode = 0; }
            if (const char *fs = getenv("JCE_DYNRES_SCALE")) {
                float v = (float)atof(fs);
                if (v >= 0.25f && v <= 1.0f) { s_mode = 2; s_fixed = v; }
            }
        }
        /* s_scale is QUANTIZED to 0.1 steps and only moves on SUSTAINED load, so
         * a transient GPU spike never resizes the target (each resize is a RT
         * realloc = a hitch).  This is the anti-flicker/anti-hitch core: the
         * viewport holds a stable resolution and steps only when the load is
         * genuinely sustained, then settles. */
        static float s_scale = 1.0f;
        static int   s_over = 0, s_under = 0;
        if (s_mode == 2) {
            s_scale = s_fixed;
        } else if (s_mode == 1) {
            JceGpuStats gs;
            if (jce_renderer_get_gpu_stats(&gs) && gs.valid && gs.gpu_ms > 0.01) {
                const double target = 16.0;           /* ~60 FPS whole-frame GPU */
                const double lo     = target * 0.65;  /* grow only well below     */
                if (gs.gpu_ms > target)  { s_over++;  s_under = 0; }
                else if (gs.gpu_ms < lo) { s_under++; s_over  = 0; }
                else                     { s_over = 0; s_under = 0; } /* dead zone: hold */
                /* 3 sustained over-target frames shrink a step; 12 sustained
                 * under-lo frames grow one back (asymmetric: quick to relieve a
                 * real hitch, slow to restore res so a light scene doesn't hunt). */
                if (s_over >= 3 && s_scale > 0.551f)       { s_scale -= 0.1f; s_over = 0; }
                else if (s_under >= 12 && s_scale < 0.999f) { s_scale += 0.1f; s_under = 0; }
                if (s_scale < 0.55f) s_scale = 0.55f;
                if (s_scale > 1.0f)  s_scale = 1.0f;
            }
        } else {
            s_scale = 1.0f;
        }
        if (s_scale < 0.999f) {
            uint32_t nw = (uint32_t)((float)width * s_scale);
            uint32_t nh = (uint32_t)((float)height * s_scale);
            if (nw >= 16u && nh >= 16u) { width = nw; height = nh; }
        }
    }

    s_cap_w = (uint16_t)width;
    s_cap_h = (uint16_t)height;

    /* Re-apply content policy when either the project or active VFS changes.
     * Explicit scene-load hooks normally do this before entity creation; this
     * check is a cheap guard for project dialogs and future mount call sites. */
    {
        const JceProject *gp = jce_editor_project_get();
        const char *groot = (gp && gp->project_root) ? gp->project_root : "";
        JceFileSystem *active_fs = jce_fs_get_active();
        JceFsActivePolicy active_policy = jce_fs_get_active_policy();
        if (active_fs != s_content_context_fs ||
            active_policy != s_content_context_policy ||
            strcmp(groot, s_content_context_project_root) != 0) {
            sr_apply_content_context();
        }
    }

    s_sr.viewport_width = width;
    s_sr.viewport_height = height;
    s_sr.camera_cache_valid = false;
    s_sr.postfx_output_tex = UINT16_MAX;
    s_sr.present_tex = UINT16_MAX;

    uint64_t now_ticks = jce_time_perf_counter();
    float dt_sec = 0.0f;
    if (s_sr.anim_last_ticks > 0) {
        dt_sec = (float)(now_ticks - s_sr.anim_last_ticks)
               / (float)jce_time_perf_freq();
        if (dt_sec > 0.1f) dt_sec = 0.1f;
    }
    s_sr.anim_last_ticks = now_ticks;

    /* Headless overview-capture hook: env JCE_DBG_OVERVIEW=1 frames the whole
     * world from a bird's-eye angle once, on the first scene-render frame, so a
     * KPI screenshot can verify the HLOD overview. Inert when unset; the camera
     * only moves on this one armed call (then the focus-anim runs to completion
     * over ~0.6s — give the KPI capture enough frames before the shot). The
     * editor frame loop separately foregrounds the Scene View tab (a docked
     * background tab never runs its body, so this hook would otherwise sit
     * dormant). */
    {
        static bool s_overview_dbg_done = false;
        if (!s_overview_dbg_done) {
            const char *ov = getenv("JCE_DBG_OVERVIEW");
            if (ov && ov[0] == '1') {
                jce_editor_scene_frame_overview();
            }
            s_overview_dbg_done = true;
        }
    }

    /* Headless eye-level vista hook (env JCE_DBG_VISTA=1): one-shot camera move
     * to a fixed vista over the meadow so a JCE_KPI_SHOT readback captures the
     * look (grass + backdrop + sky) for autonomous color QA. */
    {
        static bool s_vista_dbg_done = false;
        if (!s_vista_dbg_done) {
            const char *vs = getenv("JCE_DBG_VISTA");
            if (vs && vs[0] == '1')
                jce_editor_scene_frame_vista();
            s_vista_dbg_done = true;
        }
    }

    ed_vp_mark("evp_head", &_vpt);
    jce_editor_scene_camera_update(dt_sec);
    ed_vp_mark("evp_camera", &_vpt);

    float aspect = (float)width / (float)height;

    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, s_sr.homogeneous_depth);
    memcpy(s_sr.cached_view, view.raw[0], sizeof(s_sr.cached_view));
    memcpy(s_sr.cached_proj, proj.raw[0], sizeof(s_sr.cached_proj));

    /* TAA (r.taa; the DEFAULT is tier >= MEDIUM && has_discrete_gpu, see
     * jce_scene_renderer.c's cvar registration -- so it is ON, not off, on any
     * discrete-GPU machine.  This said "default OFF", which is the kind of
     * stale sentence that turns a measurement into a wrong conclusion: it made
     * a 19% frame-to-frame residual difference look like it could not be TAA.)
     * When active, sub-pixel-jitter the colour
       pass's projection and arm the engine PostFX pipeline's TAA resolve.
       When OFF this returns false and leaves color_proj == proj, so the
       prepare/render path is byte-identical to the legacy FXAA path. The
       clean (un-jittered) view+proj are kept for the postfx reproject and
       for end_frame's history record. */
    jce_mat4 color_proj = proj;
    bool taa_on = false;
    if (s_sr.scene_renderer) {
        taa_on = jce_scene_renderer_taa_begin_frame(s_sr.scene_renderer,
                                                    width, height,
                                                    &view, &proj, &color_proj);
        /* STANDARD per-object motion vectors: when TAA is on, ask the renderer
         * to write a per-object/per-bone velocity buffer in its depth pre-pass
         * (gated => zero cost when TAA is off).  jce_scene_renderer_render then
         * binds it into the scene PostFX TAA resolve automatically, so moving /
         * skinned characters stop ghosting. */
        jce_scene_renderer_set_taa_velocity_enabled(s_sr.scene_renderer, taa_on);
    }

    /* TSR (JCE_TSR=1): temporal super-resolution upscale of the dynamic-resolution
     * render. Jitter the colour projection ourselves (Halton) even though TAA is
     * off on the iGPU, so successive low-res frames carry distinct sub-pixel
     * offsets to accumulate into a native-res history. v1 accumulates across a
     * STATIC view only: feedback -> 0 whenever the clean view/proj changes (no
     * motion reprojection yet), so history is only trusted pixel-for-pixel. Only
     * when dynres is downscaling and TAA isn't already jittering. Opt-in; RCAS
     * stays the default upscale. */
    /* r.upscaler (LIVE): 0=Off/bilinear, 1=RCAS, 2=TSR. Read the cvar each frame
     * so the Render Pipeline panel dropdown A/Bs instantly on the same scene.
     * JCE_TSR / JCE_RCAS env forced the boot value at cvar registration. */
    static JceCvar *s_cv_up = NULL;
    static bool     s_cv_up_tried = false;
    if (!s_cv_up_tried) { s_cv_up = jce_cvar_find("r.upscaler"); s_cv_up_tried = true; }
    int   upscaler     = s_cv_up ? jce_cvar_get_int(s_cv_up) : 1;
    bool  tsr_active   = false;
    float tsr_jit_u    = 0.0f, tsr_jit_v = 0.0f;
    float tsr_feedback = 0.0f;
    float tsr_inv_vp[16] = {0};   /* inverse of current clean view*proj  */
    float tsr_prev_vp[16] = {0};  /* previous frame's clean view*proj    */
    bool  tsr_has_prev = false;   /* prev VP available -> motion reproject */
    if (upscaler == 2 && !taa_on && width < disp_w && height < disp_h) {
        static JceTaaState s_tsr_state = {};
        static float       s_tsr_prev_vp[16];
        static bool        s_tsr_prev_valid = false;
        tsr_active = true;
        jce_taa_advance(&s_tsr_state, width, height);
        jce_taa_apply_jitter(&color_proj, s_tsr_state.current_jitter);
        tsr_jit_u = s_tsr_state.current_jitter[0] * 0.5f;   /* NDC -> render-UV */
        tsr_jit_v = s_tsr_state.current_jitter[1] * 0.5f;
        tsr_feedback = 0.9f;   /* base; the shader tapers by motion speed */
        /* Clean (un-jittered) VP + its inverse drive the motion-vector pass so
         * the history reprojects under camera movement (v2). Roll prev<-cur. */
        jce_mat4 clean_vp = jce_m4_multiply(&proj, &view);
        jce_mat4 inv_vp   = jce_m4_inverse(&clean_vp);
        memcpy(tsr_inv_vp, inv_vp.raw[0], sizeof(tsr_inv_vp));
        if (s_tsr_prev_valid) {
            memcpy(tsr_prev_vp, s_tsr_prev_vp, sizeof(tsr_prev_vp));
            tsr_has_prev = true;
        }
        memcpy(s_tsr_prev_vp, clean_vp.raw[0], sizeof(s_tsr_prev_vp));
        s_tsr_prev_valid = true;
    }

    /* TSR v3: drive the scene renderer's per-object velocity prepass
     * (gbuffer_vel) when TSR is active so moving/skinned geometry reprojects,
     * not just the camera. TAA is off on the iGPU, so the taa_on-gated enable
     * above leaves it off — turn it on here for TSR. This adds a full-scene
     * velocity geometry pass — cheap for typical scenes (~0.4ms CPU / negligible
     * GPU at ~200 entities, measured) but the vertex/submit side scales with
     * entity count, so it stays OPT-IN (a perf feature shouldn't add a scaling
     * pass by default): JCE_TSR_VELOCITY=1 enables it. Default = camera-only v2,
     * the neighbourhood clamp bounding animated-object ghosting. */
    static int s_tsr_vel = -1;
    if (s_tsr_vel < 0) {
        const char *v = getenv("JCE_TSR_VELOCITY");
        s_tsr_vel = (v && v[0] != '0') ? 1 : 0;
    }
    if (tsr_active && s_tsr_vel && s_sr.scene_renderer)
        jce_scene_renderer_set_taa_velocity_enabled(s_sr.scene_renderer, true);

    {
        jce_vec3 eye = jce_camera_get_position(s_sr.camera);
        s_sr.cached_eye[0] = eye.x;
        s_sr.cached_eye[1] = eye.y;
        s_sr.cached_eye[2] = eye.z;
    }
    s_sr.camera_cache_valid = true;

    JceSceneViewMode view_mode = jce_state_get_view_mode();
    bool is_plain_wireframe = (view_mode == JCE_VIEW_WIREFRAME);

    uint32_t clear_color = is_plain_wireframe
        ? 0x373737FF
        : BG_COLOR_RGBA;

    if (!jce_offscreen_target_prepare(
            s_sr.bridge,
            width,
            height,
            view.raw[0],
            color_proj.raw[0],   /* jittered when r.taa on; == proj when off */
            clear_color,
            "EditorScene")) {
        return;
    }

    ed_vp_mark("evp_target", &_vpt);
    jce_editor_scene_asset_cache_finalize();
    ed_vp_mark("evp_assetcache", &_vpt);

    /* Engine renders sky, shadows, entities, and PostFX into the bridge view.
     * PostFX is driven via the engine's pipeline (same one the panel controls). */
    JceSceneRenderConfig cfg = jce_scene_render_config_default();

    /* Map editor view mode to engine config. */
    switch (view_mode) {
    case JCE_VIEW_WIREFRAME:
        cfg.view_mode = JCE_SCENE_VIEW_WIREFRAME; break;
    case JCE_VIEW_TEXTURED:
        cfg.view_mode = JCE_SCENE_VIEW_TEXTURED; break;
    case JCE_VIEW_WIREFRAME_TEXTURED:
        cfg.view_mode = JCE_SCENE_VIEW_WIREFRAME_TEXTURED; break;
    case JCE_VIEW_NORMALS:
        cfg.view_mode = JCE_SCENE_VIEW_NORMALS; break;
    case JCE_VIEW_ROUGHNESS:
        cfg.view_mode = JCE_SCENE_VIEW_ROUGHNESS; break;
    case JCE_VIEW_METALLIC:
        cfg.view_mode = JCE_SCENE_VIEW_METALLIC; break;
    case JCE_VIEW_AO:
        cfg.view_mode = JCE_SCENE_VIEW_AO; break;
    case JCE_VIEW_SCENE_DEPTH:
        cfg.view_mode = JCE_SCENE_VIEW_SCENE_DEPTH; break;
    case JCE_VIEW_SHADOW_CASCADES:
        cfg.view_mode = JCE_SCENE_VIEW_SHADOW_CASCADES; break;
    case JCE_VIEW_SHADOW_MASK:
        cfg.view_mode = JCE_SCENE_VIEW_SHADOW_MASK; break;
    case JCE_VIEW_SHADED:
    default:
        cfg.view_mode = JCE_SCENE_VIEW_SHADED; break;
    }

    /* Headless test hook: JCE_DBG_VIEW_MODE=<name> forces one view mode.
     *
     * This comment used to say "the default editor scene view is TEXTURED".
     * It is not, and has not been for as long as jce_editor_state.cpp has read
     * the session file: both the parse fallback and the no-session default are
     * JCE_VIEW_SHADED (= 0), which is what a capture reports as vm=0.  The
     * sentence cost real time -- it was read as evidence that an author edits
     * in an unlit view, in the middle of an investigation into whether the
     * Scene view lights meshes the way the game does.
     *
     * What is true, and is why the hook exists: TEXTURED's missing-albedo
     * checker routes every model through a SOLO draw (instancing is skipped),
     * so a headless stress taken in THAT mode never exercises the gpu-scene
     * instanced-model / Hi-Z path.  Naming SHADED explicitly pins the mode
     * rather than inheriting whatever the session file happens to hold. */
    if (const char *vm = std::getenv("JCE_DBG_VIEW_MODE")) {
        /* Every view mode reachable by name.
         *
         * A debug view only a human clicking a menu can select cannot be part
         * of a measurement, and a PARTIAL table is worse than none: the first
         * version accepted shaded/depth/cascades/shadowmask, so asking for
         * "normals" silently rendered the default view and the capture looked
         * like a normals buffer that happened to be shaded. Longest match
         * first, because "shaded", "shadowcascades" and "shadowmask" share a
         * prefix, and an unrecognised name is refused rather than ignored. */
        static const struct { const char *name; JceSceneViewModeKind mode; } kModes[] = {
            { "wireframetextured", JCE_SCENE_VIEW_WIREFRAME_TEXTURED },
            { "shadowcascades",    JCE_SCENE_VIEW_SHADOW_CASCADES    },
            { "shadowmask",        JCE_SCENE_VIEW_SHADOW_MASK        },
            { "scenedepth",        JCE_SCENE_VIEW_SCENE_DEPTH        },
            { "cascades",          JCE_SCENE_VIEW_SHADOW_CASCADES    },
            { "wireframe",         JCE_SCENE_VIEW_WIREFRAME          },
            { "roughness",         JCE_SCENE_VIEW_ROUGHNESS          },
            { "metallic",          JCE_SCENE_VIEW_METALLIC           },
            { "textured",          JCE_SCENE_VIEW_TEXTURED           },
            { "normals",           JCE_SCENE_VIEW_NORMALS            },
            { "shaded",            JCE_SCENE_VIEW_SHADED             },
            { "depth",             JCE_SCENE_VIEW_SCENE_DEPTH        },
            { "mask",              JCE_SCENE_VIEW_SHADOW_MASK        },
            { "ao",                JCE_SCENE_VIEW_AO                 },
        };
        bool matched = false;
        for (const auto &m : kModes) {
            if (std::strcmp(vm, m.name) == 0) { cfg.view_mode = m.mode; matched = true; break; }
        }
        if (!matched)
            std::fprintf(stderr, "JCE_DBG_VIEW_MODE=%s is not a view mode; "
                                 "leaving the current view\n", vm);
    }

    /* Debug views bypass post-processing -- ALL of them, from NORMALS on.
     *
     * These three write a colour that MEANS something -- a cascade index, a
     * shadow factor, a depth bucket -- and tonemapping plus exposure plus the
     * gamma encode turn it into a different colour. That is fine for a picture
     * and fatal for a readout: the first attempt to count cascade coverage from
     * this view classified 0.29% of the ground because the tints it was looking
     * for had been graded away. A view whose colours are data must deliver the
     * data.
     *
     * Extended to cover the material channels (normals, roughness, metallic,
     * AO) as well, which had been shipping through the tonemapper since they
     * were added. A normal encoded as N*0.5+0.5 and then graded is not a
     * normal any more -- it still LOOKS like a normal buffer, which is why
     * nobody noticed, and it cannot be decoded back to a direction. The first
     * attempt to use it here to hold N.L constant would have measured the
     * tonemapper. */
    if (cfg.view_mode >= JCE_SCENE_VIEW_NORMALS)
        cfg.apply_postfx = false;

    /* Wire the editor's Show menu flags into engine config so toggles
       actually take effect. */
    if (!jce_state_show_flag(JCE_SHOW_FLAG_SKYBOX))
        cfg.draw_skybox = false;
    if (!jce_state_show_flag(JCE_SHOW_FLAG_LIGHT_ICONS) &&
        !jce_state_show_flag(JCE_SHOW_FLAG_CAMERA_ICONS))
        cfg.draw_sprites = false;

    /* 0.5.7 ordering: sky → grid → entities. Engine renders sky first,
     * then invokes this callback to draw grid INTO THE SAME view, then
     * proceeds with entity submission. Grid is NOT gated by view mode
     * in 0.5.7 — it draws in wireframe modes too. */
    cfg.on_after_sky = [](uint16_t /*view_id*/, bool /*sky_drawn*/, void * /*ud*/) {
        if (!jce_state_get_show_grid()) return;
        if (jce_state_get_play_state() != JCE_PLAY_STOPPED) return;
        draw_grid();
    };
    cfg.on_after_sky_ud = nullptr;

    /* Bridge FBO, panel resolution, volumetric fog, SSR and GI — the plumbing
     * that must be identical on both editor render paths (see
     * jce_editor_viewport_common.h). */
    jce_editor_viewport_apply_shared_config(&cfg, s_sr.bridge, width, height,
                                            jce_state_get_scene());
    ed_vp_mark("evp_cfg", &_vpt);

    cfg.viewport_id = 1;   /* Scene viewport slot (Game = 0): own TAA prev camera */

    /* Two-pass GPU-query occlusion culling. Falls back to always-visible
     * when hardware queries are unsupported (ES2/WebGL1). */
    cfg.occlusion_culler = s_sr.occlusion_culler;

    /* Focus-bounded entity collection ("draw distance"): only entities within
     * cull_radius (horizontal) of the orbit target are collected, so every
     * downstream renderer pass (cull cache, shadow casters, depth/velocity
     * prepass, color pass) becomes O(near) instead of O(all entities).  This
     * is what keeps a full-loaded big world (e.g. 17k entities) playable in
     * Scene View.  Focus = the orbit target (the point the user is looking at;
     * stable under orbit/zoom and at ground level — same point used to drive
     * world streaming below). */
    cfg.cull_focus_enabled = true;
    cfg.cull_focus_x = s_sr.orbit_target.x;
    cfg.cull_focus_y = s_sr.orbit_target.y;
    cfg.cull_focus_z = s_sr.orbit_target.z;
    /* Draw distance GROWS with zoom-out: at street level the orbit distance is
     * small so this stays 900 m (full-load worlds stay playable), but a
     * bird's-eye Overview pulls the camera kilometres back — the focus radius
     * must then reach the whole world so the cheap always-resident HLOD proxies
     * render instead of being culled to bare ground (frustum + occlusion
     * culling still trim the off-screen set).  Mirrors the shadow_far
     * grow-with-view fix.  Only ever grown, never shrunk below 900 m. */
    cfg.cull_radius  = s_sr.orbit_distance * 1.25f;
    if (cfg.cull_radius < 900.0f) cfg.cull_radius = 900.0f;

    /* HDR bridge → keep the tonemap pass always-on. This MUST be set BEFORE
       jce_scene_renderer_render: the PBR shader's linear-output flag
       (u_iblParams.w) is derived from "tonemap enabled" inside that call, so the
       scene must emit LINEAR into the RGBA16F target; the always-on tonemap pass
       below then maps that linear HDR back to LDR for display (otherwise the
       viewport shows raw washed-out HDR, and smooth light falloff keeps banding
       into rings). No-op on the RGBA8 fallback. */
    if (s_sr.scene_renderer) {
        JcePostFXPipeline *pf =
            jce_scene_renderer_get_postfx(s_sr.scene_renderer);
        if (pf) {
            /* Relocate the scene postfx above the scene view's effect range
             * (scene base + JCE_VIEW_POST_BASE) so SSAO (base+2/+3) and SSR
             * (base+17/+18) coexist below it.  Default is the absolute
             * JCE_VIEW_POST_BASE, which == base+17 for the scene base (3) and
             * would collide with SSR. */
            jce_postfx_set_view_base(pf, (uint16_t)(
                scene_view_id() + JCE_EDITOR_VP_POSTFX_BASE_OFFSET));
            /* The "HDR target => force tonemap" rule used to live here, before
             * jce_scene_renderer_render.  That call rewrites every postfx
             * enable flag from scene_rendering->postfx_enabled[], so the force
             * was stomped and never took effect -- dead code that read as
             * protection.  It now runs after the render, next to any_effect,
             * matching the runtime and the Game View. */
        }
    }

    JceScene *scene = jce_state_get_scene();
    if (scene && s_sr.scene_renderer) {
        /* Ambient override from the editor Lighting panel — applied on both
         * viewports so panel changes reach whichever one renders. */
        jce_editor_viewport_apply_ambient_override(s_sr.scene_renderer);
        /* PLANAR REFLECTION FIRST, and that ordering is not cosmetic.
         *
         * It is a SECOND jce_scene_renderer_render, and the renderer's
         * per-frame state (entity list, cull, view setup) belongs to whoever
         * called it last.  Running it AFTER the main render left that state
         * describing the mirrored camera, and the viewport composited a blank
         * frame -- caught by the capture harness's own blank-frame guard, and
         * confirmed by ablating this one call.
         *
         * The reflection is still consumed one frame late either way: its
         * view id is above every scene base, so bgfx executes it after the
         * colour pass that samples it.  Running it first costs nothing and
         * leaves the main render owning the state. */
        jce_scene_renderer_render_planar_reflection(s_sr.scene_renderer, scene,
                                                     s_sr.camera, dt_sec);
        jce_scene_renderer_render(s_sr.scene_renderer, scene, s_sr.camera,
                                  scene_view_id(), dt_sec, &cfg);
        ed_vp_mark("evp_scene_render", &_vpt);

        /* Forensic occlusion KPI (JCE_KPI_OCCLUSION_LOG=1): log the scene-view
         * culler stats every 60 frames so a headless A/B run can confirm the
         * occluded count is now >0 (occlusion no longer inert in the offscreen
         * bridge path) and compare ON vs OFF (JCE_ENABLE_OCCLUSION). */
        static int s_occ_log = -1;
        if (s_occ_log < 0)
            s_occ_log = (getenv("JCE_KPI_OCCLUSION_LOG") != nullptr) ? 1 : 0;
        if (s_occ_log) {
            static unsigned s_occ_frame = 0;
            if ((s_occ_frame++ % 60u) == 0u) {
                JceSceneOcclusionStats ocs = {};
                jce_scene_renderer_get_occlusion_stats(s_sr.scene_renderer, &ocs);
                LOG_INFO(LOG_TAG,
                    "[occ-kpi scene] mode=%s tested=%u visible=%u occluded=%u "
                    "warm_up=%u no_result=%u", ocs.enabled ? "ON" : "OFF",
                    ocs.total, ocs.visible, ocs.occluded, ocs.warm_up,
                    ocs.no_result);
            }
        }
    }

    /* GPU pick pass — on-demand inside jce_scene_pick_render: it early-outs
     * unless a click request is awaiting service, so this per-frame call
     * costs a flag test on idle frames (the former every-frame full-scene
     * ID render was the editor's single largest fixed frame cost). */
    if (scene && s_sr.pick_pass) {
        /* Full-res pick (disp_w/disp_h), NOT the scaled render size: the click
         * coords are avail-space, so a native-res ID buffer keeps selection
         * pixel-exact even when the color pass is downscaled by dynamic res. */
        jce_scene_pick_render(s_sr.pick_pass, scene, s_sr.camera,
                              disp_w, disp_h);
        ed_vp_mark("evp_pick", &_vpt);
    }

    /* Composite volumetric fog (base+16) and SSR (base+19) into the bridge
     * color RT — after the scene draws into it, before overlays / PostFX run.
     * The scene renderer has already filled the depth buffer, the fog RT and
     * the SSR RT; here they are blended into the bridge.  The postfx chain was
     * relocated to base+JCE_VIEW_POST_BASE so base+16/+19 stay free below it. */
    jce_editor_viewport_composite_fog_ssr(s_sr.scene_renderer, s_sr.bridge,
                                          scene_view_id(), cfg.fog_enabled);
    const bool fullscreen_composited =
        jce_editor_viewport_apply_fullscreen_effects(
            s_sr.scene_renderer, scene, s_sr.camera, s_sr.bridge,
            scene_view_id(), 1, width, height, dt_sec);
    ed_vp_mark("evp_composite", &_vpt);

    /* Tick the world streamer each frame so pending chunk loads are applied
       to the scene synchronously on the main/render thread. */
    if (s_sr.world_streamer) {
        /* Stream around the camera's FOCUS POINT (orbit target), NOT the camera
         * position.  The editor orbit camera sweeps large arcs and rises high
         * above ground during orbit/zoom; streaming off its position made
         * chunks load/unload on every navigation (visible flicker) and emptied
         * the world entirely when zoomed out (loaded->0).  The orbit target is
         * the point the user is looking at — stable under orbit + zoom (only a
         * pan moves it) and at ground level (so the 3D distance test isn't
         * inflated by camera height).  Result: the focused area stays resident
         * (no flicker) and its streamed objects stay selectable; panning to a
         * new area streams it in.  Play uses the player position (see
         * jce_editor_play.cpp) so gameplay streaming is unaffected. */
        jce_vec3 focus = s_sr.orbit_target;
        jce_world_streamer_update(s_sr.world_streamer, focus);
        ed_vp_mark("evp_streamer", &_vpt);
        /* If preview-streaming just unloaded a chunk the user had a streamed
         * object selected from, drop the now-dead id so the gizmo/inspector
         * never touch it (mirrors the Play-tick prune). */
        jce_state_prune_dead();
        ed_vp_mark("evp_prune", &_vpt);
    }

    /* ── 0.5.7 ordering: scene → overlays → PostFX ─────────────────────
     * Overlays MUST render BEFORE PostFX into the bridge FBO (which has
     * a depth buffer). PostFX FBOs are color-only — routing overlays
     * into the postfx output silently fails depth tests for ghost &
     * selection. PostFX then tonemaps the entire composited bridge image.
     * This matches 0.5.7 exactly. */

    /* Editor overlay passes — submit into the bridge FBO (scene_view_id()
     * resolves to bridge view since s_view_id_override is unset). Grid is
     * NOT drawn here — it runs via on_after_sky callback BEFORE entities,
     * mirroring 0.5.7 ordering exactly. */
    if (fullscreen_composited &&
        jce_offscreen_target_prepare_overlay_view(
            s_sr.bridge,
            (uint16_t)(scene_view_id() + JCE_EDITOR_VP_OVERLAY_OFFSET),
            view.raw[0], proj.raw[0], "EditorSceneOverlay")) {
        s_view_id_override = (uint16_t)(
            scene_view_id() + JCE_EDITOR_VP_OVERLAY_OFFSET);
    }

    ed_vp_mark("evp_render", &_vpt);
    draw_selection_outlines();
    ed_vp_mark("evp_outlines", &_vpt);
    if (jce_state_get_show_physics_debug()) {
        draw_physics_debug();
        /* Selected compound collider's fitted wireframe is part of the collider
         * overlay — only show it when that overlay is toggled on, so selecting
         * an object with the overlay OFF shows just the selection outline. */
        draw_compound_collider_gizmos();
    }
    if (jce_state_get_show_joint_gizmos()) {
        draw_joint_gizmos();
    }
    if (jce_state_get_show_cloth_gizmos()) {
        draw_cloth_gizmos();
    }
    draw_navmesh_overlay();
    ed_vp_mark("evp_navmesh", &_vpt);
    draw_streaming_overlay();
    ed_vp_mark("evp_streaming", &_vpt);
    draw_hover_highlight();
    ed_vp_mark("evp_hover", &_vpt);
    draw_ghost_entity();
    ed_vp_mark("evp_ghost", &_vpt);
    s_view_id_override = UINT16_MAX;

    /* Apply the engine PostFX pipeline AFTER overlays so they receive
     * tonemapping along with the scene (matches 0.5.7 behavior). */
    JcePostFXPipeline *postfx = jce_scene_renderer_get_postfx(s_sr.scene_renderer);
    if (postfx) {
        /* TSR is the anti-aliaser (jittered temporal accumulation resolves
         * aliasing), so FXAA must NOT run first — it would pre-blur the very
         * samples TSR reconstructs (the classic double-AA anti-pattern). Disable
         * FXAA for this scene-view apply when TSR is active, then restore below so
         * pick / preview / thumbnail invocations keep their FXAA. */
        bool tsr_fxaa_saved = false;
        static int s_tsr_keep_fxaa = -1;   /* QA A/B: JCE_TSR_KEEP_FXAA=1 keeps FXAA on */
        if (s_tsr_keep_fxaa < 0)
            s_tsr_keep_fxaa = getenv("JCE_TSR_KEEP_FXAA") ? 1 : 0;
        if (tsr_active && !s_tsr_keep_fxaa) {
            tsr_fxaa_saved = jce_postfx_is_enabled(postfx, JCE_POSTFX_FXAA);
            if (tsr_fxaa_saved) jce_postfx_enable(postfx, JCE_POSTFX_FXAA, false);
        }
        /* The "HDR target => force tone mapping" guard used to sit here.  It is
         * gone on purpose, and NOT because it was in the wrong place.
         *
         * 2e5174e3 moved it after jce_scene_renderer_render so the
         * renderer's per-frame rewrite of scene_rendering->postfx_enabled[]
         * could no longer stomp it.  That made it live -- and live, it
         * overrides what a scene explicitly authored.  Its own rationale
         * was that an untonemapped HDR target reads washed out; bisected
         * against real content, forcing it ON is what washes out:
         *
         *   caged_kingdom/graveyard, same camera, PNG size as a proxy
         *     ec933eb8 (guard dead)  1,126,840 -> correct
         *     2e5174e3 (guard live)  1,494,787 -> washed out
         *
         * and the split across projects is exactly postfx.tonemap:
         * elemental_serenity and space author it TRUE (forcing it is a
         * no-op there, and they always looked right), while caged_kingdom
         * and street_demo author it FALSE and had their exposure and
         * lights tuned that way.  A new scene defaults to false, which is
         * why every newly created scene came up white too.
         *
         * Silently overriding an explicit authored value to protect
         * against a hypothetical is the wrong trade: honour the scene.  A
         * scene that wants tone mapping says so. */

        bool any_effect = jce_editor_viewport_postfx_any_effect(postfx);

        jce_postfx_resize(postfx, width, height);

        /* Run the post-fx chain when any effect is on OR TAA is active: the TAA
         * RESOLVE lives inside jce_postfx_apply and must run every frame TAA
         * jitters the projection, otherwise the jittered (un-resolved) frame is
         * shown directly → the whole image shimmers/crawls.  jce_postfx_apply
         * already handles the TAA-only case (its resolve becomes the output). */
        if (any_effect || taa_on) {
            JceTextureHandle scene_color = { UINT16_MAX };
            JceTextureHandle scene_depth = { UINT16_MAX };
            scene_color.idx = jce_offscreen_target_get_color_texture(s_sr.bridge);
            scene_depth.idx = jce_offscreen_target_get_depth_texture(s_sr.bridge);

            if (!jce_gfx_texture_valid(scene_color)) {
                LOG_WARN(LOG_TAG, "post-fx skipped: invalid bridge color texture");
            } else {
                jce_postfx_apply(postfx, scene_color, scene_depth);

                JceTextureHandle out = jce_postfx_get_output(postfx);
                if (jce_gfx_texture_valid(out))
                    s_sr.postfx_output_tex = out.idx;
            }
        }
        if (tsr_active && tsr_fxaa_saved)
            jce_postfx_enable(postfx, JCE_POSTFX_FXAA, true);   /* restore */
    }

    /* Fold the tone-mapped PostFX output back into the bridge so the canvas
     * UI (below) lands AFTER post-fx — same scene→postfx→UI compositing the
     * Game View and the shipped runtime use.  Editor gizmo overlays stay
     * pre-postfx above (they need the bridge depth buffer). */
    /* Declare this viewport's own view ids so the engine's ownership guard can
     * see them.  It could not before: the ECS-UI overlay below sat on top of
     * the scene renderer's dynamic-CSM atlas for as long as it took a user to
     * notice the UI was gone, and the guard written for that exact class of
     * bug reported nothing, because the editor never declared anything. */
    jce_editor_viewport_claim_view_band("editor-scene-view", scene_view_id());

    const bool postfx_composited = jce_editor_viewport_composite_postfx(
        s_sr.bridge, scene_view_id(), s_sr.postfx_output_tex, width, height);
    if (postfx_composited)
        s_sr.postfx_output_tex = UINT16_MAX; /* bridge is now the final frame */

    /* ── ECS-UI (Canvas) overlay — scene-view parity with the game view ──
     * Unity renders scene-space UI in the Scene View too; gate on the
     * UI show flag (View > Show Flags > UI, default ON).  Draw-only:
     * pointer=NULL so no button/input state machines run — the Game View
     * owns interaction.  With PostFX active the overlay draws after the
     * composite pass above (crisp, un-tonemapped UI, matching the Game
     * View and the runtime); without PostFX it draws right after the scene
     * on the mirror of the game view's base+17 slot. */
    if (s_sr.ui_canvas && jce_state_show_flag(JCE_SHOW_FLAG_UI)) {
        JceScene *ui_scene = jce_state_get_scene();
        if (ui_scene) {
            uint16_t ui_view = jce_editor_viewport_ui_overlay_view(
                scene_view_id(), postfx_composited);
            uint16_t ui_fb   = jce_offscreen_target_get_frame_buffer(s_sr.bridge);
            jce_ui_canvas_render(s_sr.ui_canvas, ui_scene, ui_view, ui_fb,
                                 (float)width, (float)height, NULL, dt_sec);
        }
    }
    ed_vp_mark("evp_uicanvas", &_vpt);

    /* Dynamic-resolution RCAS resolve: when the scene rendered below native
     * (width/height < the panel's disp size), the bridge now holds the fully
     * composited scaled frame.  Resolve it into the native-size present target
     * with contrast-adaptive sharpening (RCAS/FSR1/CAS parity) so the panel
     * displays 1:1 crisp instead of letting ImGui bilinear-stretch the scaled
     * bridge.  Only the real Scene View panel (no view-id override — overlay /
     * preview / thumbnail reuse must not resolve).  JCE_RCAS=0 forces the plain
     * bilinear path (A/B + QA); JCE_RCAS_FLIP is a QA-only V-flip override. */
    if (s_sr.present && s_view_id_override == UINT16_MAX &&
        width < disp_w && height < disp_h && tsr_active) {
        /* TSR temporal upscale (opt-in via JCE_TSR): reconstruct native res from
         * the jittered scaled bridge by depositing sub-pixel samples into an
         * output-res history, reprojected by camera motion (v2). Owns its own
         * history buffers (no s_sr.present target). Uses base+25 (motion) and
         * base+26 (resolve) — after the screenshot readback (+24). */
        JcePostFXPipeline *pfx = jce_scene_renderer_get_postfx(s_sr.scene_renderer);
        JceTextureHandle src = {
            jce_offscreen_target_get_color_texture(s_sr.bridge) };
        JceTextureHandle depth = {
            jce_offscreen_target_get_depth_texture(s_sr.bridge) };
        uint16_t base = (uint16_t)(scene_view_id() +
                                   JCE_EDITOR_VP_TSR_OFFSET);
        bool flip = false;
        if (const char *f = getenv("JCE_TSR_FLIP")) flip = (f[0] != '0');
        /* v3: per-object velocity from the scene renderer's gbuffer_vel prepass
         * (enabled above). When valid it drives the reprojection (moving/skinned
         * geometry stops ghosting); otherwise the resolve falls back to
         * camera-only motion from depth + the VP matrices (v2). */
        JceTextureHandle ext_motion = {
            jce_scene_renderer_get_velocity_texture(s_sr.scene_renderer) };
        if (pfx && jce_gfx_texture_valid(src)) {
            uint16_t out = jce_postfx_tsr_resolve(
                pfx, base, src, depth,
                tsr_has_prev ? tsr_inv_vp  : NULL,
                tsr_has_prev ? tsr_prev_vp : NULL,
                ext_motion,
                width, height, disp_w, disp_h,
                tsr_jit_u, tsr_jit_v, tsr_feedback, flip);
            if (out != UINT16_MAX) s_sr.present_tex = out;
        }
    } else if (upscaler == 1 && s_sr.present && s_view_id_override == UINT16_MAX &&
        width < disp_w && height < disp_h) {
        {
            /* No flip: vs_postfx already normalises orientation for
             * offscreen->offscreen passes (its GLSL V-flip), so the resolve is
             * an identity copy of the bridge on every backend — verified
             * right-side up on D3D11 (top-left) and OpenGL (bottom-left).
             * JCE_RCAS_FLIP overrides for QA only. */
            bool flip = false;
            if (const char *f = getenv("JCE_RCAS_FLIP")) flip = (f[0] != '0');

            jce_mat4 id = jce_m4_identity();
            if (jce_offscreen_target_prepare(s_sr.present, disp_w, disp_h,
                                             id.raw[0], id.raw[0],
                                             0x000000FFu, "EditorUpscale")) {
                JcePostFXPipeline *pfx =
                    jce_scene_renderer_get_postfx(s_sr.scene_renderer);
                JceTextureHandle src = {
                    jce_offscreen_target_get_color_texture(s_sr.bridge) };
                uint16_t dst_fb =
                    jce_offscreen_target_get_frame_buffer(s_sr.present);
                uint16_t rv = (uint16_t)(
                    scene_view_id() + JCE_EDITOR_VP_UPSCALE_OFFSET);
                if (pfx && jce_gfx_texture_valid(src) && dst_fb != UINT16_MAX &&
                    jce_postfx_upscale_resolve(pfx, rv, dst_fb, src,
                                               width, height, disp_w, disp_h,
                                               flip)) {
                    s_sr.present_tex =
                        jce_offscreen_target_get_color_texture(s_sr.present);
                }
            }
        }
    }

    /* TAA end-of-frame: record the UN-JITTERED camera for next frame's
       reproject and DISABLE TAA on the shared pipeline so it never leaks into
       the pick / preview / thumbnail postfx invocations.  Self-no-ops when
       r.taa is OFF (byte-identical). */
    if (s_sr.scene_renderer)
        jce_scene_renderer_taa_end_frame(s_sr.scene_renderer, &view, &proj);
}

/* ── Accessors ────────────────────────────────────────────────────── */

uint16_t jce_editor_scene_render_get_texture(void)
{
    if (!s_sr.initialized || !s_sr.bridge)
        return UINT16_MAX;

    /* Dynamic-resolution RCAS upscale produced a native-res frame this frame:
     * display it 1:1 instead of the scaled bridge (ImGui would bilinear-blur). */
    if (s_sr.present_tex != UINT16_MAX)
        return s_sr.present_tex;

    if (s_sr.postfx_output_tex != UINT16_MAX)
        return s_sr.postfx_output_tex;

    return jce_offscreen_target_get_color_texture(s_sr.bridge);
}

bool jce_editor_scene_render_screenshot(const char *path)
{
    if (!s_sr.initialized || !path || !path[0] || !s_sr.scene_renderer)
        return false;
    /* Headless-capable capture: read back the texture the Scene View displays
     * via blit + bgfx_read_texture.  This needs NO foreground present (unlike
     * bgfx_request_screen_shot, which never fires for a background window), so
     * it works for autonomous/headless capture.  With PostFX active the final
     * frame lives in the BRIDGE (postfx output + canvas UI composited back,
     * see the render loop); without PostFX the bridge holds it directly —
     * either way the bridge is the display source.  Poll
     * jce_editor_scene_render_capture_poll() each frame until it completes. */
    if (s_cap_w == 0 || s_cap_h == 0)
        return false;
    return jce_editor_viewport_screenshot_submit(
        s_sr.bridge, scene_view_id(), s_sr.postfx_output_tex,
        s_cap_w, s_cap_h, path);
}

/* Pump the in-flight read-back capture (no-op when idle).  Call once per frame. */
int jce_editor_scene_render_capture_poll(void)
{
    return jce_renderer_readback_capture_poll();
}

/* Panel-local px -> the canvas's own space.  The canvas was rendered at the
 * scene render-target size, which is not the panel's displayed size whenever
 * dynamic resolution is scaling; asking the canvas for its last size keeps the
 * two from drifting instead of recomputing the scale here. */
static bool ed_ui_canvas_scale(float avail_w, float avail_h,
                               float *out_sx, float *out_sy)
{
    if (!s_sr.ui_canvas || avail_w <= 0.0f || avail_h <= 0.0f) return false;
    float cw = 0.0f, ch = 0.0f;
    jce_ui_canvas_last_size(s_sr.ui_canvas, &cw, &ch);
    if (cw <= 0.0f || ch <= 0.0f) return false;
    *out_sx = cw / avail_w;
    *out_sy = ch / avail_h;
    return true;
}

uint32_t jce_editor_scene_ui_pick(float local_x, float local_y,
                                  float avail_w, float avail_h)
{
    float sx = 1.0f, sy = 1.0f;
    if (!ed_ui_canvas_scale(avail_w, avail_h, &sx, &sy)) return 0;
    if (!jce_state_show_flag(JCE_SHOW_FLAG_UI)) return 0;   /* hidden => not pickable */
    uint64_t e = jce_ui_canvas_pick(s_sr.ui_canvas, local_x * sx, local_y * sy);
    return e ? jce_state_from_ecs_entity((JceEntity)e) : 0u;
}

bool jce_editor_scene_ui_entity_rect(uint32_t id, float avail_w, float avail_h,
                                     float out_rect4[4])
{
    float sx = 1.0f, sy = 1.0f;
    if (!id || !out_rect4) return false;
    if (!ed_ui_canvas_scale(avail_w, avail_h, &sx, &sy)) return false;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    if (!jce_ui_canvas_entity_rect(s_sr.ui_canvas,
                                   (uint64_t)jce_state_to_ecs_entity(id),
                                   &x, &y, &w, &h))
        return false;
    out_rect4[0] = x / sx; out_rect4[1] = y / sy;
    out_rect4[2] = w / sx; out_rect4[3] = h / sy;
    return true;
}

bool jce_editor_scene_pick_supported(void)
{
    return s_sr.pick_pass != NULL && jce_scene_pick_supported();
}

bool jce_editor_scene_pick_request(uint32_t x, uint32_t y)
{
    if (!s_sr.pick_pass)
        return false;
    return jce_scene_pick_request(s_sr.pick_pass, x, y);
}

bool jce_editor_scene_pick_poll(uint32_t *out_entity_id)
{
    if (!s_sr.pick_pass || !out_entity_id)
        return false;

    JceScenePickResult result;
    if (!jce_scene_pick_poll(s_sr.pick_pass, &result))
        return false;

    *out_entity_id = (uint32_t)result.entity;
    return true;
}

bool jce_editor_scene_pick_request_rect(uint32_t x0, uint32_t y0,
                                        uint32_t x1, uint32_t y1)
{
    if (!s_sr.pick_pass)
        return false;
    return jce_scene_pick_request_rect(s_sr.pick_pass, x0, y0, x1, y1);
}

bool jce_editor_scene_pick_poll_rect(uint32_t *out_ids, uint32_t max_ids,
                                     uint32_t *out_count)
{
    if (!s_sr.pick_pass || !out_count)
        return false;
    static JceEntity s_tmp[4096];                 /* main-thread only */
    uint32_t cap = max_ids < 4096u ? max_ids : 4096u;
    uint32_t n = 0;
    if (!jce_scene_pick_poll_rect(s_sr.pick_pass, s_tmp, cap, &n))
        return false;
    if (out_ids)
        for (uint32_t i = 0; i < n && i < max_ids; i++)
            out_ids[i] = (uint32_t)s_tmp[i];
    *out_count = n;
    return true;
}

void jce_editor_scene_set_scene_dir(const char *dir)
{
    jce_editor_scene_asset_cache_set_scene_dir(dir);
}

/* ── Ghost (drag-preview) model ────────────────────────────────────── */

void jce_editor_scene_set_ghost(const char *mesh_path,
                                float world_x, float world_y, float world_z)
{
    if (!mesh_path || mesh_path[0] == '\0') {
        s_sr.ghost_active = false;
        return;
    }
    s_sr.ghost_active = true;
    snprintf(s_sr.ghost_mesh_path, sizeof(s_sr.ghost_mesh_path), "%s", mesh_path);
    s_sr.ghost_pos[0] = world_x;
    s_sr.ghost_pos[1] = world_y;
    s_sr.ghost_pos[2] = world_z;
}

void jce_editor_scene_clear_ghost(void)
{
    s_sr.ghost_active = false;
    s_sr.ghost_mesh_path[0] = '\0';
}

void jce_editor_scene_set_hover_entity(uint32_t entity_id)
{
    s_sr.hover_entity_id = entity_id;
}

void jce_editor_scene_clear_hover_entity(void)
{
    s_sr.hover_entity_id = 0;
}

/* ── Animation query helpers ──────────────────────────────────────── */
/*
 * Phase B: skeletal animation *playback* is owned by the engine scene
 * renderer's internal model cache. To keep the editor's timeline /
 * inspector panels functional, we maintain a tiny editor-side cache
 * (declared near the top of this file) that lazy-loads the model +
 * (optional) player for query purposes. This player is independent of
 * the engine's playback player; timeline scrubbing writes its time via
 * jce_anim_player_set_time(), and the engine renderer queries the
 * scene's JceSkeletalAnimatorComponent for its own playback state.
 */

static EdQueryCacheEntry *ed_query_cache_get(const char *skeleton_path)
{
    if (!skeleton_path || skeleton_path[0] == '\0') return nullptr;

    int free_slot = -1;
    for (int i = 0; i < ED_QUERY_CACHE_MAX; ++i) {
        EdQueryCacheEntry &e = s_query_cache[i];
        if (e.used && strcmp(e.path, skeleton_path) == 0) return &e;
        if (!e.used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return nullptr;

    JceModel *m = ed_load_model_cb(skeleton_path, NULL);
    if (!m) return nullptr;

    EdQueryCacheEntry &e = s_query_cache[free_slot];
    snprintf(e.path, sizeof(e.path), "%s", skeleton_path);
    e.model  = m;
    e.player = nullptr;
    e.used   = true;

    JceSkeleton *skel = jce_model_get_skeleton(m);
    if (skel && jce_model_anim_count(m) > 0)
        e.player = jce_anim_player_create(skel);

    return &e;
}

JceAnimPlayer *jce_editor_scene_get_anim_player(const char *skeleton_path,
                                                 uint32_t entity_id)
{
    (void)entity_id;
    /* Prefer the live player driven by the scene renderer — that's the one
     * whose time advances each frame.  Fall back to the editor's standalone
     * query cache for tools that pre-load a model without rendering it. */
    if (s_sr.scene_renderer && skeleton_path && skeleton_path[0]) {
        JceAnimPlayer *p = (JceAnimPlayer *)jce_scene_renderer_get_anim_player(
            s_sr.scene_renderer, skeleton_path);
        if (p) return p;
    }
    EdQueryCacheEntry *e = ed_query_cache_get(skeleton_path);
    return e ? e->player : nullptr;
}

JceAnimSmBinding *jce_editor_scene_get_anim_sm(uint32_t entity_id)
{
    if (s_sr.scene_renderer)
        return (JceAnimSmBinding *)jce_scene_renderer_get_anim_sm(
            s_sr.scene_renderer, entity_id);
    return nullptr;
}

JceModel *jce_editor_scene_get_model(const char *skeleton_path,
                                     uint32_t entity_id)
{
    (void)entity_id;
    if (s_sr.scene_renderer && skeleton_path && skeleton_path[0]) {
        JceModel *m = (JceModel *)jce_scene_renderer_get_model(
            s_sr.scene_renderer, skeleton_path);
        if (m) return m;
    }
    EdQueryCacheEntry *e = ed_query_cache_get(skeleton_path);
    return e ? e->model : nullptr;
}
