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

    /* World streamer: cooperative mode (no background threads) so bgfx
       handles are always created on the render/main thread. */
    {
        JceScene *scene = jce_state_get_scene();
        if (scene) {
            JceFileSystem *fs = jce_fs_create();
            if (fs) {
                /* Mount the current working directory so chunks can be
                   addressed by relative path (e.g. "chunks/c0.jscene"). */
                jce_fs_mount_dir(fs, "", ".");

                JceWorldStreamConfig wsc = jce_world_stream_config_default();
                wsc.single_thread = true; /* editor: cooperative, main-thread only */

                JceWorldStreamer *ws = jce_world_streamer_create(&wsc, scene, fs, NULL);
                if (ws) {
                    s_sr.world_streamer = ws;
                    s_sr.stream_fs      = fs;
                } else {
                    jce_fs_destroy(fs);
                    LOG_WARN(LOG_TAG, "world streamer creation failed (streaming disabled)");
                }
            }
        }
    }

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
            proj.raw[0],
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

    /* Tick the world streamer each frame so pending chunk loads are applied
       to the scene synchronously on the main/render thread. */
    if (s_sr.world_streamer) {
        jce_vec3 cam_pos = jce_camera_get_position(s_sr.camera);
        jce_world_streamer_update(s_sr.world_streamer, cam_pos);
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
    }
    if (jce_state_get_show_joint_gizmos()) {
        draw_joint_gizmos();
    }
    if (jce_state_get_show_cloth_gizmos()) {
        draw_cloth_gizmos();
    }
    draw_compound_collider_gizmos();
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

        if (any_effect) {
            JceTextureHandle scene_color = { UINT16_MAX };
            JceTextureHandle prev_pass = { UINT16_MAX };
            scene_color.idx = jce_offscreen_target_get_color_texture(s_sr.bridge);

            if (!jce_gfx_texture_valid(scene_color)) {
                LOG_WARN(LOG_TAG, "post-fx skipped: invalid bridge color texture");
            } else {
                jce_postfx_apply(postfx, scene_color, prev_pass);

                JceTextureHandle out = jce_postfx_get_output(postfx);
                if (jce_gfx_texture_valid(out))
                    s_sr.postfx_output_tex = out.idx;
            }
        }
    }
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
