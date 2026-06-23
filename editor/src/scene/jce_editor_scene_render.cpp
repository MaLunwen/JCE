/*
 * jce_editor_scene_render.cpp  Editor 3D scene rendering (FBO pipeline).
 *
 * Phase B refactor: scene rendering is delegated to the engine
 * `JceSceneRenderer` (sky, shadows, entities, sprites, IBL, post-fx
 * params). This file owns the overlay pipeline (grid, selection, ghost,
 * hover, physics debug) and the editor's PostFX bloom/tonemap chain.
 */

#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_filesystem.h>

#include "io/jce_editor_file_util.h"
#include "jce_scene_render_internal.h"
#include "core/jce_assetdb.h"
#include "ui/jce_editor_panels.h"

#include <cstdio>

extern "C" {
#include <jce/middleware/animation/jce_animation.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_volumetric_fog.h>

bool jce_editor_lighting_get_fog_enabled(void);
void jce_editor_lighting_get_fog_params(JceVolumetricFogParams *out);
void jce_editor_lighting_get_ambient(float out_color_rgb[3], float *out_intensity);
}

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

/* ── Animation query cache (forward state for shutdown) ───────────── */

#define ED_QUERY_CACHE_MAX 16
struct EdQueryCacheEntry {
    char           path[256];
    JceModel      *model;
    JceAnimPlayer *player;
    bool           used;
};
static EdQueryCacheEntry s_query_cache[ED_QUERY_CACHE_MAX];

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
    s_sr.bridge = jce_offscreen_target_create(renderer,
                                                  (uint16_t)JCE_VIEW_EDITOR_SCENE);
    if (!s_sr.bridge) {
        LOG_WARN(LOG_TAG, "failed to create editor render bridge");
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

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
    cbs.texture_failed  = ed_texture_failed_cb;
    cbs.resolve_path    = ed_resolve_path_cb;
    cbs.userdata        = NULL;
    s_sr.scene_renderer = jce_scene_renderer_create(renderer, pak, &cbs);
    if (!s_sr.scene_renderer) {
        LOG_WARN(LOG_TAG, "failed to create engine scene renderer");
        if (s_sr.camera) { jce_camera_destroy(s_sr.camera); s_sr.camera = NULL; }
        if (s_sr.bridge) { jce_offscreen_target_destroy(s_sr.bridge); s_sr.bridge = NULL; }
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

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
     * occlusion queries are unsupported (ES2 / WebGL1). */
    {
        JceShaderSet oc_shaders;
        memset(&oc_shaders, 0, sizeof(oc_shaders));
        JceShaderHandle ch = jce_renderer_get_program_color(renderer);
        oc_shaders.color.idx = ch.idx;

        JceOcclusionConfig oc_cfg = jce_occlusion_config_default();
        s_sr.occlusion_culler = jce_occlusion_culler_create(&oc_cfg, &oc_shaders);
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
    jce_fs_mount_dir(fs, "", base);

    JceWorldStreamConfig wsc = jce_world_stream_config_default();
    wsc.mode            = (st->mode == 1) ? JCE_STREAM_RECTANGULAR
                                          : JCE_STREAM_RADIAL;
    wsc.load_radius     = st->load_radius;
    wsc.unload_radius   = st->unload_radius;
    wsc.max_pending     = st->max_pending;
    wsc.budget_mb       = st->budget_mb;
    wsc.frame_budget_ms = st->frame_budget_ms;
    wsc.single_thread   = true; /* editor: cooperative, main-thread only */

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

void jce_editor_scene_render_frame(uint32_t width, uint32_t height)
{
    if (!s_sr.initialized || !s_sr.renderer) return;
    if (width == 0 || height == 0) return;

    s_sr.viewport_width = width;
    s_sr.viewport_height = height;
    s_sr.camera_cache_valid = false;
    s_sr.postfx_output_tex = UINT16_MAX;

    uint64_t now_ticks = jce_time_perf_counter();
    float dt_sec = 0.0f;
    if (s_sr.anim_last_ticks > 0) {
        dt_sec = (float)(now_ticks - s_sr.anim_last_ticks)
               / (float)jce_time_perf_freq();
        if (dt_sec > 0.1f) dt_sec = 0.1f;
    }
    s_sr.anim_last_ticks = now_ticks;

    jce_editor_scene_camera_update(dt_sec);

    float aspect = (float)width / (float)height;

    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, s_sr.homogeneous_depth);
    memcpy(s_sr.cached_view, view.raw[0], sizeof(s_sr.cached_view));
    memcpy(s_sr.cached_proj, proj.raw[0], sizeof(s_sr.cached_proj));

    /* TAA (r.taa, default OFF): when active, sub-pixel-jitter the colour
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

    jce_editor_scene_asset_cache_finalize();

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
    case JCE_VIEW_SHADED:
    default:
        cfg.view_mode = JCE_SCENE_VIEW_SHADED; break;
    }

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

    /* Broadphase frustum culling — uniform-grid backed. Stats appear in
     * the Profiler panel under "Scene Culling". Approximate AABBs derived
     * from transform position + scale; precise mesh AABBs are a TODO. */
    cfg.frustum_culling = true;
    cfg.viewport_width = width;
    cfg.viewport_height = height;
    cfg.viewport_id = 1;   /* Scene viewport slot (Game = 0): own TAA prev camera */

    /* Two-pass GPU-query occlusion culling. Falls back to always-visible
     * when hardware queries are unsupported (ES2/WebGL1). */
    cfg.occlusion_culler = s_sr.occlusion_culler;

    /* Volumetric fog (Stage 1: render only — composite pass deferred).
     * Lighting panel writes; renderer consumes here. */
    cfg.fog_enabled = jce_editor_lighting_get_fog_enabled();
    if (cfg.fog_enabled) {
        jce_editor_lighting_get_fog_params(&cfg.fog);
        cfg.fog_depth_tex_handle =
            jce_offscreen_target_get_depth_texture(s_sr.bridge);
        cfg.fog_rt_width  = (int)s_sr.viewport_width;
        cfg.fog_rt_height = (int)s_sr.viewport_height;
    } else {
        cfg.fog_depth_tex_handle = UINT16_MAX;
        cfg.fog_rt_width = 0;
        cfg.fog_rt_height = 0;
    }

    /* SSR: the renderer reflects the bridge's lit color RT (gated on the
     * scene's ssr_enabled).  SSR's ray-march view renders after the color
     * pass, so it samples the current frame's clean (pre-composite) color. */
    cfg.ssr_color_tex_handle =
        jce_offscreen_target_get_color_texture(s_sr.bridge);

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
    cfg.cull_radius  = 900.0f;

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
            jce_postfx_set_view_base(pf,
                (uint16_t)(scene_view_id() + JCE_VIEW_POST_BASE));
            if (jce_offscreen_target_is_hdr(s_sr.bridge))
                jce_postfx_enable(pf, JCE_POSTFX_TONEMAP, true);
        }
    }

    JceScene *scene = jce_state_get_scene();
    if (scene && s_sr.scene_renderer) {
        float amb_color[3];
        float amb_intensity = 0.15f;
        jce_editor_lighting_get_ambient(amb_color, &amb_intensity);
        jce_scene_renderer_set_ambient_override(s_sr.scene_renderer,
                                                 amb_color, amb_intensity);
        jce_scene_renderer_render(s_sr.scene_renderer, scene, s_sr.camera,
                                  scene_view_id(), dt_sec, &cfg);
    }

    /* GPU pick pass — on-demand inside jce_scene_pick_render: it early-outs
     * unless a click request is awaiting service, so this per-frame call
     * costs a flag test on idle frames (the former every-frame full-scene
     * ID render was the editor's single largest fixed frame cost). */
    if (scene && s_sr.pick_pass) {
        jce_scene_pick_render(s_sr.pick_pass, scene, s_sr.camera,
                              width, height);
    }

    /* Composite volumetric fog into the bridge color RT (after the
     * scene draws into it but before overlays / PostFX run). The
     * scene renderer has already filled the depth buffer & fog RT;
     * here we blend rgb in-scatter + transmittance into the bridge.
     * No-op when fog is disabled or composite shader unavailable. */
    if (cfg.fog_enabled && s_sr.scene_renderer) {
        uint16_t fog_composite_view = (uint16_t)(scene_view_id() + 16);
        uint16_t dst_fb = jce_offscreen_target_get_frame_buffer(s_sr.bridge);
        jce_scene_renderer_composite_fog(s_sr.scene_renderer,
                                         fog_composite_view, dst_fb);
    }

    /* Composite SSR reflections into the bridge color RT (after the scene +
     * fog, before overlays).  No-op unless SSR was active this frame.  Uses
     * scene base+3 (> the SSR ray-march at base+2 and the color pass). */
    if (s_sr.scene_renderer) {
        /* base+19 (after the base+18 ray-march); the postfx was relocated to
         * base+JCE_VIEW_POST_BASE so base+18/+19 stay free below it. */
        uint16_t ssr_composite_view = (uint16_t)(scene_view_id() + 19);
        uint16_t dst_fb = jce_offscreen_target_get_frame_buffer(s_sr.bridge);
        jce_scene_renderer_composite_ssr(s_sr.scene_renderer,
                                         ssr_composite_view, dst_fb);
    }

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
        /* If preview-streaming just unloaded a chunk the user had a streamed
         * object selected from, drop the now-dead id so the gizmo/inspector
         * never touch it (mirrors the Play-tick prune). */
        jce_state_prune_dead();
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
    draw_selection_outlines();
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
    draw_streaming_overlay();
    draw_hover_highlight();
    draw_ghost_entity();

    /* Apply the engine PostFX pipeline AFTER overlays so they receive
     * tonemapping along with the scene (matches 0.5.7 behavior). */
    JcePostFXPipeline *postfx = jce_scene_renderer_get_postfx(s_sr.scene_renderer);
    if (postfx) {
        bool any_effect = false;
        for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
            if (jce_postfx_is_enabled(postfx, (JcePostFXType)i)) {
                any_effect = true;
                break;
            }
        }

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

    if (s_sr.postfx_output_tex != UINT16_MAX)
        return s_sr.postfx_output_tex;

    return jce_offscreen_target_get_color_texture(s_sr.bridge);
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
