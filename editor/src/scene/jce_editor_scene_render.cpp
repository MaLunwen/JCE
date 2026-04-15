/*
 * jce_editor_scene_render.cpp  Editor 3D scene rendering (FBO pipeline).
 *
 * Owns the SceneRenderState instance and implements init/shutdown/frame.
 * Camera logic is in jce_scene_render_camera.cpp.
 * Draw logic (sky, grid, entities) is in jce_scene_render_draw.cpp.
 */

#include "jce_scene_render_internal.h"
#include "jce_editor_file_util.h"

/* ── State instance (shared via extern in internal header) ────────── */

SceneRenderState s_sr;

/* ── Helpers (shared via internal header) ─────────────────────────── */

uint16_t scene_view_id(void)
{
    if (s_sr.bridge)
        return jce_editor_render_bridge_get_view_id(s_sr.bridge);
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

/* ── Model / animation cache ─────────────────────────────────────── */

ModelCacheEntry *get_cached_model(const char *skeleton_path, uint32_t entity_id)
{
    if (!skeleton_path || skeleton_path[0] == '\0') return nullptr;

    /* Look up existing entry by path. */
    int free_slot = -1;
    for (int i = 0; i < MODEL_CACHE_MAX; ++i) {
        ModelCacheEntry &e = s_sr.model_cache[i];
        if (e.used && strcmp(e.path, skeleton_path) == 0) {
            e.bound_entity = entity_id;
            return &e;
        }
        if (!e.used && free_slot < 0) free_slot = i;
    }

    if (free_slot < 0) return nullptr; /* cache full */

    /* Load from disk. */
    size_t fsize = 0;
    void *buf = ed_read_file(skeleton_path, &fsize);
    if (!buf) {
        LOG_WARN(LOG_TAG, "model cache: cannot read %s", skeleton_path);
        return nullptr;
    }

    JceModel *model = jce_model_load_gltf_memory(buf, (uint32_t)fsize,
                                                   skeleton_path);
    ED_FREE(buf);
    if (!model) return nullptr;

    ModelCacheEntry &e = s_sr.model_cache[free_slot];
    snprintf(e.path, sizeof(e.path), "%s", skeleton_path);
    e.model        = model;
    e.player       = nullptr;
    e.bound_entity = entity_id;
    e.active_clip  = -1;
    e.loop         = false;
    e.speed        = 1.0f;
    e.paused       = true;
    e.used         = true;

    /* Create anim player if skeleton + clips are available. */
    JceSkeleton *skel = jce_model_get_skeleton(model);
    if (skel && jce_model_anim_count(model) > 0) {
        e.player = jce_anim_player_create(skel);
    }

    return &e;
}

/* ── Init ─────────────────────────────────────────────────────────── */

bool jce_editor_scene_render_init(JceRenderer *renderer,
                                  const PakArchive *pak,
                                  JceAssetManager *assets)
{
    if (s_sr.initialized) return true;

    memset(&s_sr, 0, sizeof(s_sr));
    jce_editor_scene_asset_cache_init(assets);
    s_sr.white_tex.idx = UINT16_MAX;
    s_sr.checker_tex.idx = UINT16_MAX;
    s_sr.renderer = renderer;
    s_sr.bridge = jce_editor_render_bridge_create(renderer,
                                                  (uint16_t)JCE_VIEW_EDITOR_SCENE);
    if (!s_sr.bridge) {
        LOG_WARN(LOG_TAG, "failed to create editor render bridge");
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

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
            jce_editor_render_bridge_destroy(s_sr.bridge);
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

    /* Pos + color vertex layout for transient buffers (grid, sky). */
    bgfx_vertex_layout_begin(&s_sr.layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&s_sr.layout, BGFX_ATTRIB_COLOR0, 4,
                           BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&s_sr.layout);

    /* Cache the color shader program handle. */
    JceShaderHandle sh = jce_renderer_get_program_color(renderer);
    s_sr.prog_color.idx = sh.idx;

    /* Load sky/grid shader programs from the PAK archive. */
    JceShaderHandle sky_sh = shader_load_program(pak, "sky");
    s_sr.prog_sky.idx = sky_sh.idx;
    if (sky_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "sky shader not found in PAK — sky will be skipped");
    JceShaderHandle grid_sh = shader_load_program(pak, "grid");
    s_sr.prog_grid.idx = grid_sh.idx;
    if (grid_sh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "grid shader not found in PAK — grid will be skipped");

    /* Position-only vertex layout for the fullscreen sky quad. */
    bgfx_vertex_layout_begin(&s_sr.sky_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&s_sr.sky_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&s_sr.sky_layout);

    /* Uniforms for sky gradient and fullscreen grid. */
    s_sr.u_sky_colors = bgfx_create_uniform("u_sky_colors",
                                             BGFX_UNIFORM_TYPE_VEC4, 3);
    s_sr.u_grid_camera = bgfx_create_uniform("u_grid_camera",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_grid_fade = bgfx_create_uniform("u_grid_fade",
                                           BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Cache lighting uniform handles for flat-color selection outlines. */
    s_sr.u_light_dir   = bgfx_create_uniform("u_lightDir",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_light_color = bgfx_create_uniform("u_lightColor",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Procedural meshes. */
    s_sr.cube_mesh     = jce_mesh_create_cube(1.0f);
    s_sr.plane_mesh    = jce_mesh_create_plane(1.0f, 1.0f, 0);
    s_sr.sphere_mesh   = jce_mesh_create_sphere(0.5f);
    s_sr.capsule_mesh  = jce_mesh_create_capsule(0.25f, 1.0f);
    s_sr.cylinder_mesh = jce_mesh_create_cylinder(0.5f, 1.0f);

    /* 1x1 white fallback texture for SHADED mode. */
    {
        uint32_t white = 0xFFFFFFFF;
        const bgfx_memory_t *mem = bgfx_copy(&white, 4);
        s_sr.white_tex = bgfx_create_texture_2d(1, 1, false, 1,
                                                  BGFX_TEXTURE_FORMAT_RGBA8, 0, mem);
    }

    /* 8x8 magenta/black checkerboard for missing textures (TEXTURED mode). */
    {
        const uint32_t M = 0xFFFF00FF;
        const uint32_t K = 0xFF000000;
        uint32_t checker[8 * 8];
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                checker[y * 8 + x] = ((x ^ y) & 1) ? K : M;
        const bgfx_memory_t *cmem = bgfx_copy(checker, sizeof(checker));
        s_sr.checker_tex = bgfx_create_texture_2d(8, 8, false, 1,
                                                    BGFX_TEXTURE_FORMAT_RGBA8,
                                                    BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                                                    cmem);
    }

    /* Shadow map resources. */
    {
        const uint16_t shadow_size = 2048;
        s_sr.shadow_tex = bgfx_create_texture_2d(
            shadow_size, shadow_size, false, 1,
            BGFX_TEXTURE_FORMAT_D16,
            BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            NULL);
        bgfx_attachment_t at;
        memset(&at, 0, sizeof(at));
        bgfx_attachment_init(&at, s_sr.shadow_tex, BGFX_ACCESS_WRITE,
                             0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
        s_sr.shadow_fbo = bgfx_create_frame_buffer_from_attachment(1, &at, false);
        s_sr.u_shadowMap = bgfx_create_uniform("s_shadowMap",
                                                BGFX_UNIFORM_TYPE_SAMPLER, 1);
        s_sr.u_shadowVP  = bgfx_create_uniform("u_shadowVP",
                                                BGFX_UNIFORM_TYPE_MAT4, 1);
        s_sr.shadow_valid = BGFX_HANDLE_IS_VALID(s_sr.shadow_fbo);
        if (s_sr.shadow_valid)
            LOG_INFO(LOG_TAG, "shadow map created (%dx%d)", shadow_size, shadow_size);
    }

    /* Multi-light environment. */
    s_sr.light_env = jce_light_env_create();

    s_sr.initialized = true;
    LOG_INFO(LOG_TAG, "editor scene renderer initialized (FBO pipeline)");
    return true;
}

/* ── Shutdown ─────────────────────────────────────────────────────── */

void jce_editor_scene_render_shutdown(void)
{
    if (!s_sr.initialized) return;

    /* Flush model / animation cache. */
    for (int i = 0; i < MODEL_CACHE_MAX; ++i) {
        ModelCacheEntry &e = s_sr.model_cache[i];
        if (!e.used) continue;
        if (e.player) { jce_anim_player_destroy(e.player); e.player = nullptr; }
        if (e.model)  { jce_model_destroy(e.model);        e.model  = nullptr; }
        e.used = false;
    }

    jce_editor_scene_asset_cache_shutdown();

    if (s_sr.bridge) {
        jce_editor_render_bridge_destroy(s_sr.bridge);
        s_sr.bridge = NULL;
    }

    if (s_sr.camera)     { jce_camera_destroy(s_sr.camera);   s_sr.camera = NULL; }
    if (s_sr.cube_mesh)     { jce_mesh_destroy(s_sr.cube_mesh);     s_sr.cube_mesh = NULL; }
    if (s_sr.plane_mesh)    { jce_mesh_destroy(s_sr.plane_mesh);    s_sr.plane_mesh = NULL; }
    if (s_sr.sphere_mesh)   { jce_mesh_destroy(s_sr.sphere_mesh);   s_sr.sphere_mesh = NULL; }
    if (s_sr.capsule_mesh)  { jce_mesh_destroy(s_sr.capsule_mesh);  s_sr.capsule_mesh = NULL; }
    if (s_sr.cylinder_mesh) { jce_mesh_destroy(s_sr.cylinder_mesh); s_sr.cylinder_mesh = NULL; }

    if (BGFX_HANDLE_IS_VALID(s_sr.white_tex))
        bgfx_destroy_texture(s_sr.white_tex);
    if (BGFX_HANDLE_IS_VALID(s_sr.checker_tex))
        bgfx_destroy_texture(s_sr.checker_tex);

    if (BGFX_HANDLE_IS_VALID(s_sr.prog_sky))
        bgfx_destroy_program(s_sr.prog_sky);
    if (BGFX_HANDLE_IS_VALID(s_sr.prog_grid))
        bgfx_destroy_program(s_sr.prog_grid);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_sky_colors))
        bgfx_destroy_uniform(s_sr.u_sky_colors);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_camera))
        bgfx_destroy_uniform(s_sr.u_grid_camera);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_grid_fade))
        bgfx_destroy_uniform(s_sr.u_grid_fade);

    if (BGFX_HANDLE_IS_VALID(s_sr.shadow_fbo))
        bgfx_destroy_frame_buffer(s_sr.shadow_fbo);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_shadowMap))
        bgfx_destroy_uniform(s_sr.u_shadowMap);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_shadowVP))
        bgfx_destroy_uniform(s_sr.u_shadowVP);

    if (s_sr.light_env) {
        jce_light_env_destroy(s_sr.light_env);
        s_sr.light_env = NULL;
    }

    s_sr.initialized = false;
    LOG_INFO(LOG_TAG, "editor scene renderer shutdown");
}

/* ── Per-frame ────────────────────────────────────────────────────── */

void jce_editor_scene_render_frame(uint32_t width, uint32_t height)
{
    if (!s_sr.initialized || !s_sr.renderer) return;
    if (width == 0 || height == 0) return;

    const bgfx_caps_t *caps = bgfx_get_caps();
    float aspect = (float)width / (float)height;

    jce_mat4 view = jce_camera_view(s_sr.camera);
    jce_mat4 proj = jce_camera_proj(s_sr.camera, aspect, caps->homogeneousDepth);

    JceSceneViewMode view_mode = jce_state_get_view_mode();

    uint32_t clear_color = (view_mode == JCE_VIEW_WIREFRAME)
        ? 0x373737FF
        : BG_COLOR_RGBA;

    if (!jce_editor_render_bridge_prepare(
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

    if (view_mode != JCE_VIEW_WIREFRAME)
        draw_sky_gradient();

    if (jce_state_get_show_grid())
        draw_grid();

    draw_entities();
    draw_hover_highlight();
    draw_ghost_entity();
}

/* ── Accessors ────────────────────────────────────────────────────── */

uint16_t jce_editor_scene_render_get_texture(void)
{
    if (!s_sr.initialized || !s_sr.bridge)
        return UINT16_MAX;
    return jce_editor_render_bridge_get_color_texture(s_sr.bridge);
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

JceAnimPlayer *jce_editor_scene_get_anim_player(const char *skeleton_path,
                                                 uint32_t entity_id)
{
    ModelCacheEntry *mc = get_cached_model(skeleton_path, entity_id);
    return mc ? mc->player : nullptr;
}

JceModel *jce_editor_scene_get_model(const char *skeleton_path,
                                     uint32_t entity_id)
{
    ModelCacheEntry *mc = get_cached_model(skeleton_path, entity_id);
    return mc ? mc->model : nullptr;
}
