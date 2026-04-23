/*
 * jce_editor_scene_render.cpp  Editor 3D scene rendering (FBO pipeline).
 *
 * Phase B refactor: scene rendering is delegated to the engine
 * `JceSceneRenderer` (sky, shadows, entities, sprites, IBL, post-fx
 * params). This file owns the overlay pipeline (grid, selection, ghost,
 * hover, physics debug) and the editor's PostFX bloom/tonemap chain.
 */

#include "jce_scene_render_internal.h"
#include "jce_editor_file_util.h"

#include <SDL3/SDL_timer.h>

extern "C" {
#include <jce/graphics/jce_postfx.h>
#include <jce/graphics/jce_model.h>
#include <jce/animation/jce_animation.h>
#include <jce/scene/jce_scene.h>
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
    size_t fsize = 0;
    void *buf = ed_read_file(path, &fsize);
    if (!buf) return NULL;
    JceModel *m = jce_model_load_gltf_memory(buf, (uint32_t)fsize, path);
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

    const bgfx_caps_t *caps = bgfx_get_caps();
    s_sr.homogeneous_depth = caps ? caps->homogeneousDepth : false;

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
    bgfx_vertex_layout_begin(&s_sr.layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_COLOR0, 4,
                           BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&s_sr.layout);

    /* Cache the color shader program handle. */
    JceShaderHandle sh = jce_renderer_get_program_color(renderer);
    s_sr.prog_color.idx = sh.idx;

    /* Load editor-only grid shader from the PAK archive. */
    JceShaderHandle grid_sh = shader_load_program(pak, "grid");
    s_sr.prog_grid.idx = grid_sh.idx;
    if (grid_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "grid shader not found in PAK — grid will be skipped");

    s_sr.u_grid_camera = bgfx_create_uniform("u_grid_camera",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_grid_fade = bgfx_create_uniform("u_grid_fade",
                                           BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Lighting uniforms for flat-color overlay (selection / ghost / hover). */
    s_sr.u_light_dir   = bgfx_create_uniform("u_lightDir",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_light_color = bgfx_create_uniform("u_lightColor",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);

    /* 1x1 white fallback texture for overlay binding. */
    {
        uint32_t white = 0xFFFFFFFF;
        const bgfx_memory_t *mem = bgfx_copy(&white, 4);
        s_sr.white_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                                  BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* Create the engine scene renderer with editor asset callbacks. */
    JceSceneRendererCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.load_mesh    = ed_load_mesh_cb;
    cbs.load_model   = ed_load_model_cb;
    cbs.load_texture = ed_load_texture_cb;
    cbs.userdata     = NULL;
    s_sr.scene_renderer = jce_scene_renderer_create(renderer, pak, &cbs);
    if (!s_sr.scene_renderer) {
        LOG_WARN(LOG_TAG, "failed to create engine scene renderer");
        if (s_sr.camera) { jce_camera_destroy(s_sr.camera); s_sr.camera = NULL; }
        if (s_sr.bridge) { jce_offscreen_target_destroy(s_sr.bridge); s_sr.bridge = NULL; }
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

    s_sr.anim_last_ticks = 0;
    s_sr.initialized = true;
    LOG_INFO(LOG_TAG, "editor scene renderer initialized (engine-backed)");
    return true;
}

/* ── Shutdown ─────────────────────────────────────────────────────── */

void jce_editor_scene_render_shutdown(void)
{
    if (!s_sr.initialized) return;

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

    if (BGFX_HANDLE_IS_VALID(s_sr.white_tex))
        bgfx_destroy_texture(s_sr.white_tex);

    if (BGFX_HANDLE_IS_VALID(s_sr.prog_grid))
        bgfx_destroy_program(s_sr.prog_grid);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_camera))
        bgfx_destroy_uniform(s_sr.u_grid_camera);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_fade))
        bgfx_destroy_uniform(s_sr.u_grid_fade);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_light_dir))
        bgfx_destroy_uniform(s_sr.u_light_dir);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_light_color))
        bgfx_destroy_uniform(s_sr.u_light_color);

    s_sr.initialized = false;
    LOG_INFO(LOG_TAG, "editor scene renderer shutdown");
}

JceSceneRenderer *jce_editor_get_scene_renderer(void)
{
    return s_sr.scene_renderer;
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

    /* Compute frame delta time for skeletal animation. */
    uint64_t now_ticks = SDL_GetPerformanceCounter();
    float dt_sec = 0.0f;
    if (s_sr.anim_last_ticks > 0) {
        dt_sec = (float)(now_ticks - s_sr.anim_last_ticks)
               / (float)SDL_GetPerformanceFrequency();
        if (dt_sec > 0.1f) dt_sec = 0.1f;
    }
    s_sr.anim_last_ticks = now_ticks;

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

    JceScene *scene = jce_state_get_scene();
    if (scene && s_sr.scene_renderer) {
        jce_scene_renderer_render(s_sr.scene_renderer, scene, s_sr.camera,
                                  scene_view_id(), dt_sec, &cfg);
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
    EdQueryCacheEntry *e = ed_query_cache_get(skeleton_path);
    return e ? e->player : nullptr;
}

JceModel *jce_editor_scene_get_model(const char *skeleton_path,
                                     uint32_t entity_id)
{
    (void)entity_id;
    EdQueryCacheEntry *e = ed_query_cache_get(skeleton_path);
    return e ? e->model : nullptr;
}
