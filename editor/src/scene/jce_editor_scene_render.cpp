/*
 * jce_editor_scene_render.cpp  Editor 3D scene rendering (FBO pipeline).
 *
 * Owns the SceneRenderState instance and implements init/shutdown/frame.
 * Camera logic is in jce_scene_render_camera.cpp.
 * Draw logic (sky, grid, entities) is in jce_scene_render_draw.cpp.
 */

#include "jce_scene_render_internal.h"
#include "jce_editor_file_util.h"

extern "C" {
#include <jce/graphics/jce_postfx.h>
}

extern JcePostFXPipeline *g_editor_postfx;

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
                                  const JcePakArchive *pak,
                                  JceAssetManager *assets)
{
    if (s_sr.initialized) return true;

    memset(&s_sr, 0, sizeof(s_sr));
    jce_editor_scene_asset_cache_init(assets);
    s_sr.white_tex.idx = UINT16_MAX;
    s_sr.checker_tex.idx = UINT16_MAX;
    s_sr.postfx_output_tex = UINT16_MAX;
    s_sr.shadow_use_csm = false;
    s_sr.shadow_far_valid = false;
    s_sr.renderer = renderer;
    s_sr.bridge = jce_editor_render_bridge_create(renderer,
                                                  (uint16_t)JCE_VIEW_EDITOR_SCENE);
    if (!s_sr.bridge) {
        LOG_WARN(LOG_TAG, "failed to create editor render bridge");
        jce_editor_scene_asset_cache_shutdown();
        return false;
    }

    const bgfx_caps_t *caps = bgfx_get_caps();
    s_sr.homogeneous_depth = caps ? caps->homogeneousDepth : false;

    /* Select best available depth format for shadow maps.
     * D32F > D24S8 > D16 — higher precision reduces shadow banding. */
    bgfx_texture_format_t shadow_depth_fmt = BGFX_TEXTURE_FORMAT_D16;
    if (caps) {
        uint16_t d32f = caps->formats[BGFX_TEXTURE_FORMAT_D32F];
        uint16_t d24  = caps->formats[BGFX_TEXTURE_FORMAT_D24S8];
        if ((d32f & BGFX_CAPS_FORMAT_TEXTURE_2D)
            && (d32f & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
            shadow_depth_fmt = BGFX_TEXTURE_FORMAT_D32F;
        else if ((d24 & BGFX_CAPS_FORMAT_TEXTURE_2D)
                 && (d24 & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
            shadow_depth_fmt = BGFX_TEXTURE_FORMAT_D24S8;
    }

    /* Dynamic shadow quality defaults from GPU tier recommendation. */
    {
        JceRenderRecommendation rec = jce_renderer_get_recommendation();
        uint32_t shadow_size = rec.shadow_map_size;
        if (shadow_size < 1024) shadow_size = 1024;
        if (shadow_size > 4096) shadow_size = 4096;
        s_sr.shadow_map_size = (uint16_t)shadow_size;

        switch (rec.tier) {
        case JCE_GPU_TIER_HIGH:
            s_sr.csm_blend_ratio  = 0.22f;
            s_sr.csm_normal_bias  = 0.015f;
            s_sr.csm_filter_radius = 1.6f;
            break;
        case JCE_GPU_TIER_MEDIUM:
            s_sr.csm_blend_ratio  = 0.20f;
            s_sr.csm_normal_bias  = 0.012f;
            s_sr.csm_filter_radius = 1.4f;
            break;
        case JCE_GPU_TIER_LOW:
        default:
            s_sr.csm_blend_ratio  = 0.18f;
            s_sr.csm_normal_bias  = 0.010f;
            s_sr.csm_filter_radius = 1.2f;
            break;
        }
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
    s_sr.orbit_clip_valid = false;
    s_sr.camera_cache_valid = false;

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
    s_sr.u_sky_params = bgfx_create_uniform("u_sky_params",
                                             BGFX_UNIFORM_TYPE_VEC4, 1);
    s_sr.u_sky_equirect = bgfx_create_uniform("s_equirect",
                                               BGFX_UNIFORM_TYPE_SAMPLER, 1);
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
        const uint16_t shadow_size = s_sr.shadow_map_size;
        s_sr.shadow_tex = bgfx_create_texture_2d(
            shadow_size, shadow_size, false, 1,
            shadow_depth_fmt,
            BGFX_TEXTURE_RT
            | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
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

    /* Cascaded shadow maps (4 cascades at 2048x2048). */
    {
        const uint32_t csm_count = JCE_CSM_MAX_CASCADES;
        const uint16_t csm_size = s_sr.shadow_map_size;
        const char *sampler_names[JCE_CSM_MAX_CASCADES] = {
            "s_csmShadow0", "s_csmShadow1", "s_csmShadow2", "s_csmShadow3"
        };
        s_sr.csm_cascade_count = csm_count;
        s_sr.csm_valid = true;
        for (uint32_t i = 0; i < csm_count; i++) {
            s_sr.csm_tex[i] = bgfx_create_texture_2d(
                csm_size, csm_size, false, 1,
                shadow_depth_fmt,
                BGFX_TEXTURE_RT
                | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
                NULL);
            bgfx_attachment_t at;
            memset(&at, 0, sizeof(at));
            bgfx_attachment_init(&at, s_sr.csm_tex[i], BGFX_ACCESS_WRITE,
                                 0, 1, 0, BGFX_RESOLVE_AUTO_GEN_MIPS);
            s_sr.csm_fbo[i] = bgfx_create_frame_buffer_from_attachment(1, &at, false);
            s_sr.u_csm_samplers[i] = bgfx_create_uniform(
                sampler_names[i], BGFX_UNIFORM_TYPE_SAMPLER, 1);
            if (!BGFX_HANDLE_IS_VALID(s_sr.csm_fbo[i]))
                s_sr.csm_valid = false;
        }
        s_sr.u_csm_vp     = bgfx_create_uniform("u_csmVP",
                                                  BGFX_UNIFORM_TYPE_MAT4,
                                                  JCE_CSM_MAX_CASCADES);
        s_sr.u_csm_splits  = bgfx_create_uniform("u_csmSplits",
                                                   BGFX_UNIFORM_TYPE_VEC4, 1);
        s_sr.u_csm_params = bgfx_create_uniform("u_csmParams",
                                                 BGFX_UNIFORM_TYPE_VEC4, 1);
        s_sr.u_csm_bias_scales = bgfx_create_uniform("u_csmBiasScales",
                                                      BGFX_UNIFORM_TYPE_VEC4, 1);
        if (s_sr.csm_valid)
            LOG_INFO(LOG_TAG, "CSM created (%u cascades, %dx%d)",
                     csm_count, csm_size, csm_size);
    }

    /* Multi-light environment. */
    s_sr.light_env = jce_light_env_create();

    /* IBL / skybox uniforms. */
    s_sr.u_ibl_irradiance = bgfx_create_uniform("s_irradiance",
                                                  BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_sr.u_ibl_prefilter  = bgfx_create_uniform("s_prefilter",
                                                  BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_sr.u_ibl_brdf_lut   = bgfx_create_uniform("s_brdfLUT",
                                                  BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s_sr.u_ibl_params     = bgfx_create_uniform("u_iblParams",
                                                  BGFX_UNIFORM_TYPE_VEC4, 1);
    JceTexture brdf = jce_ibl_create_brdf_lut(256);
    s_sr.brdf_lut         = { brdf.idx };
    s_sr.skybox           = NULL;
    s_sr.ibl_data         = NULL;
    s_sr.skybox_active    = false;
    s_sr.skybox_hdr_path[0] = '\0';

    /* Sprite batch for 2D sprite entities. */
    s_sr.sprite_batch = jce_sprite_batch_create(256);

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
    if (BGFX_HANDLE_IS_VALID(s_sr.u_sky_params))
        bgfx_destroy_uniform(s_sr.u_sky_params);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_sky_equirect))
        bgfx_destroy_uniform(s_sr.u_sky_equirect);
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

    /* CSM resources. */
    for (uint32_t i = 0; i < JCE_CSM_MAX_CASCADES; i++) {
        if (BGFX_HANDLE_IS_VALID(s_sr.csm_fbo[i]))
            bgfx_destroy_frame_buffer(s_sr.csm_fbo[i]);
        if (BGFX_HANDLE_IS_VALID(s_sr.u_csm_samplers[i]))
            bgfx_destroy_uniform(s_sr.u_csm_samplers[i]);
    }
    if (BGFX_HANDLE_IS_VALID(s_sr.u_csm_vp))
        bgfx_destroy_uniform(s_sr.u_csm_vp);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_csm_splits))
        bgfx_destroy_uniform(s_sr.u_csm_splits);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_csm_params))
        bgfx_destroy_uniform(s_sr.u_csm_params);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_csm_bias_scales))
        bgfx_destroy_uniform(s_sr.u_csm_bias_scales);

    if (s_sr.light_env) {
        jce_light_env_destroy(s_sr.light_env);
        s_sr.light_env = NULL;
    }

    /* IBL / skybox resources. */
    if (s_sr.ibl_data) {
        jce_ibl_destroy(s_sr.ibl_data);
        s_sr.ibl_data = NULL;
    }
    if (s_sr.skybox) {
        jce_skybox_destroy(s_sr.skybox);
        s_sr.skybox = NULL;
    }
    if (BGFX_HANDLE_IS_VALID(s_sr.brdf_lut))
        bgfx_destroy_texture(s_sr.brdf_lut);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_ibl_irradiance))
        bgfx_destroy_uniform(s_sr.u_ibl_irradiance);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_ibl_prefilter))
        bgfx_destroy_uniform(s_sr.u_ibl_prefilter);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_ibl_brdf_lut))
        bgfx_destroy_uniform(s_sr.u_ibl_brdf_lut);
    if (BGFX_HANDLE_IS_VALID(s_sr.u_ibl_params))
        bgfx_destroy_uniform(s_sr.u_ibl_params);

    /* Sprite batch. */
    if (s_sr.sprite_batch) {
        jce_sprite_batch_destroy(s_sr.sprite_batch);
        s_sr.sprite_batch = NULL;
    }

    s_sr.initialized = false;
    LOG_INFO(LOG_TAG, "editor scene renderer shutdown");
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
    s_sr.postfx_tonemap_active = false;
    s_sr.shadow_use_csm = false;

    if (g_editor_postfx) {
        s_sr.postfx_tonemap_active =
            jce_postfx_is_enabled(g_editor_postfx, JCE_POSTFX_TONEMAP);
    }


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
    bool is_textured_wireframe = (view_mode == JCE_VIEW_WIREFRAME_TEXTURED);

    uint32_t clear_color = is_plain_wireframe
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

    /* Scan for Skybox component and load HDR if path changed. */
    {
        const char *hdr_path = NULL;
        float sky_rotation = 0.0f;
        float sky_exposure = 1.0f;
        int ent_count = jce_state_get_entity_count();
        for (int ei = 0; ei < ent_count && !hdr_path; ei++) {
            JceEntityInfo *ent = jce_state_get_entity_by_index(ei);
            if (!ent) continue;
            int cc = 0;
            JceComponentInfo *comps = jce_state_get_entity_components(ent->id, &cc);
            for (int ci = 0; ci < cc; ci++) {
                if (comps[ci].type == JCE_COMP_SKYBOX && comps[ci].data.skybox.hdr_path[0]) {
                    hdr_path = comps[ci].data.skybox.hdr_path;
                    sky_rotation = comps[ci].data.skybox.rotation;
                    sky_exposure = comps[ci].data.skybox.exposure > 0.0f
                                 ? comps[ci].data.skybox.exposure : 1.0f;
                    break;
                }
            }
        }

        if (hdr_path && strcmp(hdr_path, s_sr.skybox_hdr_path) != 0) {
            /* Path changed — reload skybox. */
            if (s_sr.skybox) {
                jce_skybox_destroy(s_sr.skybox);
                s_sr.skybox = NULL;
            }
            s_sr.skybox = jce_skybox_create_from_hdr_file(hdr_path, 512);
            if (s_sr.skybox) {
                snprintf(s_sr.skybox_hdr_path, sizeof(s_sr.skybox_hdr_path),
                         "%s", hdr_path);
                s_sr.skybox_active = true;
                LOG_INFO(LOG_TAG, "skybox loaded: %s", hdr_path);
            } else {
                s_sr.skybox_hdr_path[0] = '\0';
                s_sr.skybox_active = false;
            }
        } else if (!hdr_path && s_sr.skybox_active) {
            /* Skybox component removed. */
            if (s_sr.skybox) {
                jce_skybox_destroy(s_sr.skybox);
                s_sr.skybox = NULL;
            }
            s_sr.skybox_hdr_path[0] = '\0';
            s_sr.skybox_active = false;
        }

        /* Store current exposure/rotation for draw_sky_gradient(). */
        s_sr.skybox_exposure = sky_exposure;
        s_sr.skybox_rotation = sky_rotation;
    }

    /* Keep the legacy gray background only for plain wireframe.
     * Wireframe-textured keeps the normal blue clear color, and only draws
     * the sky when an actual HDR skybox is active. This avoids falling back
     * to the shader's default gradient in that mode. */
    if (!is_plain_wireframe && (!is_textured_wireframe || s_sr.skybox_active)) {
        draw_sky_gradient();
    }

    if (jce_state_get_show_grid() && jce_state_get_play_state() == JCE_PLAY_STOPPED) {
        draw_grid();
    }

    draw_entities();
    draw_hover_highlight();
    draw_ghost_entity();

    if (g_editor_postfx) {
        bool any_effect = false;
        for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
            if (jce_postfx_is_enabled(g_editor_postfx, (JcePostFXType)i)) {
                any_effect = true;
                break;
            }
        }

        jce_postfx_resize(g_editor_postfx, width, height);

        if (any_effect) {
            JceTextureHandle scene_color = { UINT16_MAX };
            JceTextureHandle prev_pass = { UINT16_MAX };
            scene_color.idx = jce_editor_render_bridge_get_color_texture(s_sr.bridge);

            jce_postfx_apply(g_editor_postfx,
                             scene_color,
                             prev_pass);

            JceTextureHandle out = jce_postfx_get_output(g_editor_postfx);
            if (jce_gfx_texture_valid(out))
                s_sr.postfx_output_tex = out.idx;
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
