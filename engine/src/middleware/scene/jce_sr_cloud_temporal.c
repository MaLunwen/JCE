/* Half-resolution physical cloud integration and per-viewport history. */
#include "jce_sr_internal.h"
#include "jce_scene_internal.h"
#include "renderer/jce_cloud_noise.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include <jce/os/core/jce_console.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_shaders.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


enum { CLOUD_VIEWPORT_COUNT = 3 };

typedef struct {
    bgfx_frame_buffer_handle_t fb;
    bgfx_texture_handle_t color, distance;
} SrCloudBuffer;

typedef struct {
    SrCloudBuffer buffer[2];
    uint64_t scene_id, depth_generation;
    jce_mat4 view_proj;
    jce_vec3 position, forward, sun;
    double wind[2];
    float coverage, density, bottom, top, fov;
    uint32_t frame;
    uint16_t width, height, atlas;
    unsigned latest;
    bool valid, cumulus, layered_wind, published, opaque_composited;
    const JceCamera *published_camera;
    uint16_t source_width, source_height;
    float exposure;
} SrCloudHistory;

struct SrCloudTemporal {
    SrCloudHistory history[CLOUD_VIEWPORT_COUNT];
    bgfx_program_handle_t program, composite;
    bgfx_uniform_handle_t control, origin, previous_vp, current_vp, previous_eye, wind;
    bgfx_uniform_handle_t prev_color, prev_distance, resolved;
    bgfx_uniform_handle_t scene_depth, depth_params, layer_wind;
    bool attempted;
};

static int cloud_viewport(uint16_t base)
{
    switch (base) {
    case JCE_VIEW_EDITOR_SCENE: return 0;
    case JCE_VIEW_EDITOR_GAME: return 1;
    case JCE_VIEW_RUNTIME_GAME: return 2;
    default: return -1;
    }
}

bool sr_cloud_cumulus_enabled(void)
{
    static JceCvar *option;
    if (!option) option = jce_cvar_register_bool("r.cloud_cumulus", true, 0,
                                                "Layered cumulus in the physical sky");
    return !option || jce_cvar_get_bool(option);
}

/* Same-executable controls for fixed-camera render validation. */
static bool cloud_validation_mode(const char *name)
{
    const char *mode = getenv("JCE_CLOUD_VALIDATION_MODE");
    return mode && strcmp(mode, name) == 0;
}

static bool cloud_half_enabled(void)
{
    static JceCvar *option;
    if (!option) option = jce_cvar_register_bool("r.cloud_half_res", true, 0,
                                                "Render physical clouds at half width and height");
    return !cloud_validation_mode("full") && (!option || jce_cvar_get_bool(option));
}

static bool cloud_depth_clip_enabled(void)
{
    static JceCvar *option;
    if (!option) option = jce_cvar_register_bool("r.cloud_depth_clip", true, 0,
                                                "Clip half-resolution cloud rays by scene depth");
    return !cloud_validation_mode("depth-off") && (!option || jce_cvar_get_bool(option));
}

static bool cloud_layer_wind_enabled(void)
{
    static JceCvar *option;
    if (!option) option = jce_cvar_register_bool("r.cloud_layer_wind", true, 0,
                                                "Use separate lower and upper cloud winds");
    return !cloud_validation_mode("wind-off") && (!option || jce_cvar_get_bool(option));
}

static float cloud_history_weight(void)
{
    static JceCvar *option;
    if (!option) option = jce_cvar_register_float("r.cloud_history_weight", 0.82f, 0,
                                                  "Cloud reprojection blend, 0 disables history");
    if (cloud_validation_mode("spatial")) return 0.0f;
    const float weight = option ? jce_cvar_get_float(option) : 0.82f;
    return isfinite(weight) ? fmaxf(0.0f, fminf(weight, 0.95f)) : 0.0f;
}

static SrCloudTemporal *cloud_temporal_state(JceSceneRenderer *sr)
{
    SrCloudTemporal *p = sr->cloud_temporal;
    if (p) return p;
    p = JCE_CALLOC(1, sizeof(*p));
    if (!p) return NULL;
    for (unsigned i = 0; i < CLOUD_VIEWPORT_COUNT; ++i)
        for (unsigned j = 0; j < 2; ++j) {
            SrCloudBuffer *b = &p->history[i].buffer[j];
            b->fb.idx = b->color.idx = b->distance.idx = UINT16_MAX;
        }
    p->program.idx = p->composite.idx = UINT16_MAX;
    p->control = bgfx_create_uniform("u_cloud_temporal", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->origin = bgfx_create_uniform("u_cloud_rt_params", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->current_vp = bgfx_create_uniform("u_cloud_sample_view_proj", BGFX_UNIFORM_TYPE_MAT4, 1);
    p->previous_vp = bgfx_create_uniform("u_cloud_previous_view_proj", BGFX_UNIFORM_TYPE_MAT4, 1);
    p->previous_eye = bgfx_create_uniform("u_cloud_previous_eye", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->wind = bgfx_create_uniform("u_cloud_history_wind", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->prev_color = bgfx_create_uniform("s_cloud_previous_color", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->prev_distance = bgfx_create_uniform("s_cloud_previous_distance", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->resolved = bgfx_create_uniform("s_sky_cloud_resolved", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->scene_depth = bgfx_create_uniform("s_cloud_scene_depth", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    p->depth_params = bgfx_create_uniform("u_cloud_depth_params", BGFX_UNIFORM_TYPE_VEC4, 1);
    p->layer_wind = bgfx_create_uniform("u_cloud_layer_wind", BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->cloud_temporal = p;
    return p;
}

static void cloud_buffer_destroy(SrCloudBuffer *buffer)
{
    if (BGFX_HANDLE_IS_VALID(buffer->fb)) bgfx_destroy_frame_buffer(buffer->fb);
    if (BGFX_HANDLE_IS_VALID(buffer->color)) bgfx_destroy_texture(buffer->color);
    if (BGFX_HANDLE_IS_VALID(buffer->distance)) bgfx_destroy_texture(buffer->distance);
    buffer->fb.idx = buffer->color.idx = buffer->distance.idx = UINT16_MAX;
}

static bool cloud_buffer_create(SrCloudBuffer *buffer, uint16_t width, uint16_t height)
{
    const uint64_t clamp_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    const uint64_t point_flags = clamp_flags | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    if (!bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, clamp_flags) ||
        !bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_R32F, point_flags))
        return false;
    buffer->color = bgfx_create_texture_2d(width, height, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F, clamp_flags, NULL, 0);
    buffer->distance = bgfx_create_texture_2d(width, height, false, 1,
        BGFX_TEXTURE_FORMAT_R32F, point_flags, NULL, 0);
    if (BGFX_HANDLE_IS_VALID(buffer->color) && BGFX_HANDLE_IS_VALID(buffer->distance)) {
        const bgfx_texture_handle_t attachments[] = {buffer->color, buffer->distance};
        buffer->fb = bgfx_create_frame_buffer_from_handles(2, attachments, false);
    }
    if (BGFX_HANDLE_IS_VALID(buffer->fb)) return true;
    cloud_buffer_destroy(buffer);
    return false;
}

static bool cloud_history_resize(SrCloudHistory *h, uint16_t width, uint16_t height)
{
    if (h->width == width && h->height == height &&
        BGFX_HANDLE_IS_VALID(h->buffer[0].fb) && BGFX_HANDLE_IS_VALID(h->buffer[1].fb))
        return true;
    h->valid = false;
    cloud_buffer_destroy(&h->buffer[0]);
    cloud_buffer_destroy(&h->buffer[1]);
    if (!cloud_buffer_create(&h->buffer[0], width, height) ||
        !cloud_buffer_create(&h->buffer[1], width, height)) {
        cloud_buffer_destroy(&h->buffer[0]);
        cloud_buffer_destroy(&h->buffer[1]);
        return false;
    }
    h->width = width;
    h->height = height;
    h->latest = 0;
    return true;
}

static bool cloud_viewport_extent_valid(const JceSceneRenderConfig *cfg)
{
    return cfg && cfg->viewport_width > 0u && cfg->viewport_height > 0u &&
        cfg->viewport_width <= UINT16_MAX && cfg->viewport_height <= UINT16_MAX;
}

bool sr_cloud_temporal_wanted(JceSceneRenderer *sr, const JceSceneRenderingSettings *rs,
    const JceCamera *camera, uint16_t base, const JceSceneRenderConfig *cfg)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!sr || !rs || !camera || !caps || !cloud_viewport_extent_valid(cfg) ||
        sr->sky_legacy_gl21 || cfg->reduced_pass ||
        !cfg->draw_skybox || cfg->view_mode != JCE_SCENE_VIEW_SHADED ||
        cloud_viewport(base) < 0 || caps->limits.maxFBAttachments < 2 ||
        (unsigned)base + JCE_VIEW_SR_CLOUD_OPAQUE_OFFSET >= caps->limits.maxViews ||
        (base == JCE_VIEW_EDITOR_SCENE && caps->limits.maxViews <= JCE_VIEW_EDITOR_PICK_READBACK) ||
        jce_camera_get_mode(camera) != JCE_CAMERA_PERSPECTIVE ||
        rs->sky_mode != JCE_SCENE_SKY_PHYSICAL || rs->cloud_coverage <= 0.001f)
        return false;

    JceRenderPipelineDesc pipeline;
    jce_render_pipeline_get(&pipeline);
    return pipeline.post_quality >= JCE_RP_QUALITY_HIGH;
}

static bool cloud_temporal_program(SrCloudTemporal *p, JceSceneRenderer *sr)
{
    if (!p->attempted) {
        p->attempted = true;
        p->program.idx = jce_renderer_shader_load_named_internal(sr->pak, "sky", "cloud_temporal").idx;
        p->composite.idx = jce_renderer_shader_load_named_internal(sr->pak, "sky", "cloud_opaque").idx;
    }
    return BGFX_HANDLE_IS_VALID(p->program) && BGFX_HANDLE_IS_VALID(p->control) &&
        BGFX_HANDLE_IS_VALID(p->origin) && BGFX_HANDLE_IS_VALID(p->previous_vp) &&
        BGFX_HANDLE_IS_VALID(p->current_vp) && BGFX_HANDLE_IS_VALID(p->previous_eye) &&
        BGFX_HANDLE_IS_VALID(p->wind) && BGFX_HANDLE_IS_VALID(p->prev_color) &&
        BGFX_HANDLE_IS_VALID(p->prev_distance) && BGFX_HANDLE_IS_VALID(p->resolved) &&
        BGFX_HANDLE_IS_VALID(p->scene_depth) && BGFX_HANDLE_IS_VALID(p->depth_params) &&
        BGFX_HANDLE_IS_VALID(p->layer_wind);
}

bgfx_texture_handle_t sr_cloud_temporal_prepare_depth(JceSceneRenderer *sr,
    uint16_t base, const JceSceneRenderConfig *cfg, bool velocity, bool normals, bool admit)
{
    const bgfx_texture_handle_t unavailable = {UINT16_MAX};
    const int slot = cloud_viewport(base);
    if (sr->cloud_temporal && slot >= 0) {
        sr->cloud_temporal->history[slot].published = false;
        sr->cloud_temporal->history[slot].opaque_composited = false;
    }
    if (!admit || !sr->pak || !cloud_depth_clip_enabled() ||
        jce_renderer_get_program_shadow(sr->renderer).idx == UINT16_MAX)
        return unavailable;
    SrCloudTemporal *p = cloud_temporal_state(sr);
    if (!p || !cloud_temporal_program(p, sr)) return unavailable;
    sr_ensure_ssao_target(sr, base, cfg->viewport_width, cfg->viewport_height, velocity, normals);
    return sr->ssao_valid ? sr->ssao_depth_tex : unavailable;
}

static void cloud_upper_wind(double x, double z, double upper[2])
{
    // Rotate and accelerate the integrated base wind, keeping gusts and
    // direction changes continuous. Each layer wraps after this transform.
    upper[0] = 1.18 * x - 0.56 * z;
    upper[1] = 0.56 * x + 1.18 * z;
}

void sr_cloud_temporal_bind(JceSceneRenderer *sr, const JceScene *scene,
    const JceCamera *camera, const JceSceneRenderConfig *cfg, uint16_t base,
    const float sun_dir[4], float exposure, bool admit, bgfx_texture_handle_t scene_depth)
{
    SrCloudTemporal *p = cloud_temporal_state(sr);
    if (!p || !BGFX_HANDLE_IS_VALID(p->control)) return;
    float control[4] = {0};
    bgfx_set_uniform(p->control, control, 1);

    const int slot = cloud_viewport(base);
    if (slot < 0) return;
    SrCloudHistory *h = &p->history[slot];
    h->published = false;
    h->opaque_composited = false;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!admit || !camera || !caps || !cloud_viewport_extent_valid(cfg) ||
        !cloud_temporal_program(p, sr)) {
        h->valid = false;
        return;
    }

    const uint32_t divisor = cloud_half_enabled() ? 2u : 1u;
    const uint32_t width = (cfg->viewport_width + divisor - 1u) / divisor;
    const uint32_t height = (cfg->viewport_height + divisor - 1u) / divisor;
    if (!width || !height || width > caps->limits.maxTextureSize ||
        height > caps->limits.maxTextureSize ||
        !cloud_history_resize(h, (uint16_t)width, (uint16_t)height)) {
        h->valid = false;
        return;
    }

    const uint32_t frame = jce_renderer_get_frame_index(sr->renderer);
    const jce_mat4 view = jce_camera_view(camera);
    const float aspect = (float)cfg->viewport_width / (float)cfg->viewport_height;
    const jce_mat4 proj = jce_camera_proj(camera, aspect, sr->homogeneous_depth);
    const jce_mat4 vp = jce_m4_multiply(&proj, &view);
    const jce_vec3 eye = jce_camera_get_position(camera);
    const jce_vec3 forward = jce_camera_get_forward(camera);
    const jce_vec3 sun = {sun_dir[0], sun_dir[1], sun_dir[2]};
    const jce_vec3 shift = jce_v3_sub(eye, h->position);
    const float fov = jce_camera_get_fov(camera);
    const bool cumulus = sr_cloud_cumulus_enabled();
    const bool layered_wind = cloud_layer_wind_enabled();
    const bool clip_depth = BGFX_HANDLE_IS_VALID(scene_depth);
    const uint64_t scene_id = jce_scene_internal_instance_id(scene);
    const uint64_t depth_generation = clip_depth ? sr->ssao_target_generation : 0u;
    const double wind_x = sr->cloud_wind_offset[0] - h->wind[0];
    const double wind_z = sr->cloud_wind_offset[1] - h->wind[1];
    double upper_delta[2];
    cloud_upper_wind(wind_x, wind_z, upper_delta);
    const float weight = cloud_history_weight();
    const bool valid = weight > 0.0f && h->valid && h->scene_id == scene_id &&
        h->depth_generation == depth_generation && h->layered_wind == layered_wind &&
        frame - h->frame == 1u && h->atlas == sr->cloud_atlas_tex.idx &&
        h->cumulus == cumulus && fabsf(fov - h->fov) < 0.2f &&
        fabsf(sr->cloud_coverage - h->coverage) < 0.001f &&
        fabsf(sr->cloud_density - h->density) < 0.002f &&
        sr->cloud_bottom_km == h->bottom && sr->cloud_top_km == h->top &&
        jce_v3_dot(shift, shift) < 256.0f * 256.0f &&
        jce_v3_dot(forward, h->forward) > 0.94f &&
        jce_v3_dot(sun, h->sun) > 0.999f && fabs(wind_x) < 64.0 && fabs(wind_z) < 64.0 &&
        (!layered_wind || (fabs(upper_delta[0]) < 64.0 && fabs(upper_delta[1]) < 64.0));

    const unsigned write = h->latest ^ 1u;
    const SrCloudBuffer *prev = &h->buffer[h->latest];
    const SrCloudBuffer *next = &h->buffer[write];
    const float origin[4] = {caps->originBottomLeft ? 1.0f : 0.0f,
                             1.0f / (float)width, 1.0f / (float)height, (float)divisor};
    const float last_eye[4] = {h->position.x, h->position.y, h->position.z, 0.0f};
    const float wind[4] = {(float)wind_x, (float)wind_z,
                           (float)upper_delta[0], (float)upper_delta[1]};
    float layer_wind[4] = {0};
    if (layered_wind) {
        JceCloudNoiseParams params;
        jce_cloud_noise_params_default(&params);
        double upper[2];
        cloud_upper_wind(sr->cloud_wind_offset[0], sr->cloud_wind_offset[1], upper);
        layer_wind[0] = (float)(fmod(upper[0], params.period_x) / params.period_x);
        layer_wind[1] = (float)(fmod(upper[1], params.period_z) / params.period_z);
        layer_wind[2] = 1.0f;
    }
    const float depth_params[4] = {clip_depth ? 1.0f : 0.0f,
        sr->homogeneous_depth ? 1.0f : 0.0f,
        0.5f / (float)cfg->viewport_width, 0.5f / (float)cfg->viewport_height};
    control[1] = valid ? 1.0f : 0.0f;
    control[2] = weight > 0.0f ? (float)(frame & 15u) * (1.0f / 16.0f) : 0.0f;
    control[3] = weight;
    bgfx_set_uniform(p->control, control, 1);
    bgfx_set_uniform(p->origin, origin, 1);
    bgfx_set_uniform(p->previous_vp, valid ? h->view_proj.raw[0] : vp.raw[0], 1);
    bgfx_set_uniform(p->previous_eye, last_eye, 1);
    bgfx_set_uniform(p->wind, wind, 1);
    bgfx_set_uniform(p->layer_wind, layer_wind, 1);
    bgfx_set_uniform(p->depth_params, depth_params, 1);
    bgfx_set_texture(7, p->prev_color, prev->color, UINT32_MAX);
    bgfx_set_texture(0, p->scene_depth, clip_depth ? scene_depth : prev->distance, UINT32_MAX);
    bgfx_set_texture(6, p->prev_distance, prev->distance, UINT32_MAX);

    const uint16_t cloud_view = (uint16_t)(base + JCE_VIEW_SR_CLOUD_TEMPORAL_OFFSET);
    bgfx_set_view_name(cloud_view, "Scene/CloudTemporal", INT32_MAX);
    bgfx_set_view_rect(cloud_view, 0, 0, h->width, h->height);
    bgfx_set_view_frame_buffer(cloud_view, next->fb);
    bgfx_set_view_transform(cloud_view, view.raw[0], proj.raw[0]);
    bgfx_set_view_clear(cloud_view, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    /* The caller's sky quad and cloud/atmosphere uniforms are shared. */
    bgfx_submit(cloud_view, p->program, 0, BGFX_DISCARD_NONE);
    if (getenv("JCE_DBG_CLOUD_HISTORY"))
        LOG_INFO("scene_renderer", "cloud history frame=%u view=%u size=%ux%u reused=%u depth=%u layers=%u",
            (unsigned)frame, (unsigned)base, (unsigned)width, (unsigned)height, valid ? 1u : 0u,
            clip_depth ? 1u : 0u, layered_wind ? 2u : 1u);


    control[0] = 1.0f;
    bgfx_set_uniform(p->control, control, 1);
    bgfx_set_uniform(p->current_vp, vp.raw[0], 1);
    bgfx_set_texture(7, p->resolved, next->color, UINT32_MAX);

    // Reuse the sky quad and the bounded cloud buffer. This draw is ordered
    // after opaque colour/optics and before transparency, despite being
    // recorded here while the atmosphere bindings are still live.
    if (clip_depth && BGFX_HANDLE_IS_VALID(p->composite) &&
        cfg->scene_frame_buffer != UINT16_MAX && cfg->draw_opaque &&
        cfg->viewport_width <= UINT16_MAX && cfg->viewport_height <= UINT16_MAX) {
        const uint16_t composite_view = (uint16_t)(base + JCE_VIEW_SR_CLOUD_OPAQUE_OFFSET);
        const bgfx_frame_buffer_handle_t fb = {cfg->scene_frame_buffer};
        bgfx_set_view_name(composite_view, "Scene/CloudOpaqueComposite", INT32_MAX);
        bgfx_set_view_rect(composite_view, 0, 0,
            (uint16_t)cfg->viewport_width, (uint16_t)cfg->viewport_height);
        bgfx_set_view_frame_buffer(composite_view, fb);
        bgfx_set_view_transform(composite_view, view.raw[0], proj.raw[0]);
        bgfx_set_view_clear(composite_view, BGFX_CLEAR_NONE, 0, 1.0f, 0);
        bgfx_set_view_mode(composite_view, BGFX_VIEW_MODE_SEQUENTIAL);
        bgfx_set_state(BGFX_STATE_WRITE_RGB |
            BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_SRC_ALPHA), 0);
        bgfx_submit(composite_view, p->composite, 0, BGFX_DISCARD_NONE);
        bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA, 0);
        h->opaque_composited = true;
    }

    h->scene_id = scene_id;
    h->depth_generation = depth_generation;
    h->view_proj = vp;
    h->position = eye;
    h->forward = forward;
    h->sun = sun;
    h->wind[0] = sr->cloud_wind_offset[0];
    h->wind[1] = sr->cloud_wind_offset[1];
    h->coverage = sr->cloud_coverage;
    h->density = sr->cloud_density;
    h->bottom = sr->cloud_bottom_km;
    h->top = sr->cloud_top_km;
    h->fov = fov;
    h->frame = frame;
    h->atlas = sr->cloud_atlas_tex.idx;
    h->latest = write;
    h->valid = true;
    h->cumulus = cumulus;
    h->layered_wind = layered_wind;
    h->source_width = (uint16_t)cfg->viewport_width;
    h->source_height = (uint16_t)cfg->viewport_height;
    h->published_camera = camera;
    h->exposure = exposure;
    h->published = true;
}

bgfx_texture_handle_t sr_cloud_temporal_surface(JceSceneRenderer *sr,
    const JceScene *scene, const JceCamera *camera, uint16_t base,
    jce_mat4 *view_proj, float *exposure, bool *opaque_composited)
{
    const bgfx_texture_handle_t unavailable = {UINT16_MAX};
    const int slot = cloud_viewport(base);
    if (opaque_composited) *opaque_composited = false;
    if (!sr || !sr->cloud_temporal || !scene || !camera || !view_proj ||
        !exposure || !opaque_composited || slot < 0)
        return unavailable;
    const SrCloudHistory *h = &sr->cloud_temporal->history[slot];
    if (!h->published || !h->valid || h->published_camera != camera ||
        h->frame != jce_renderer_get_frame_index(sr->renderer) ||
        h->scene_id != jce_scene_internal_instance_id(scene) ||
        !h->source_width || !h->source_height || !isfinite(h->exposure) ||
        !BGFX_HANDLE_IS_VALID(h->buffer[h->latest].color))
        return unavailable;
    const jce_mat4 view = jce_camera_view(camera);
    const float aspect = (float)h->source_width / (float)h->source_height;
    const jce_mat4 proj = jce_camera_proj(camera, aspect, sr->homogeneous_depth);
    const jce_mat4 current = jce_m4_multiply(&proj, &view);
    if (memcmp(current.raw, h->view_proj.raw, sizeof(current.raw)) != 0)
        return unavailable;
    *view_proj = h->view_proj;
    *exposure = h->exposure;
    *opaque_composited = h->opaque_composited;
    return h->buffer[h->latest].color;
}

uint16_t sr_cloud_temporal_transparency(JceSceneRenderer *sr,
    const JceScene *scene, const JceCamera *camera, uint16_t base,
    const JceSceneRenderConfig *cfg, uint16_t existing_view)
{
    // Optics may already have configured this same transparency view.
    if (existing_view != base || !cfg || cfg->reduced_pass || !cfg->draw_transparent)
        return existing_view;
    jce_mat4 vp;
    float exposure;
    bool opaque_composited = false;
    const bgfx_texture_handle_t cloud = sr_cloud_temporal_surface(sr, scene, camera,
        base, &vp, &exposure, &opaque_composited);
    if (!BGFX_HANDLE_IS_VALID(cloud) || !opaque_composited)
        return existing_view;
    const uint16_t id = (uint16_t)(base + JCE_VIEW_SR_TRANSPARENCY_OFFSET);
    const bgfx_frame_buffer_handle_t fb = {cfg->scene_frame_buffer};
    const jce_mat4 view = jce_camera_view(camera);
    const jce_mat4 proj = jce_camera_proj(camera,
        (float)cfg->viewport_width / (float)cfg->viewport_height, sr->homogeneous_depth);
    bgfx_set_view_name(id, "Scene/Transparent", INT32_MAX);
    bgfx_set_view_rect(id, 0, 0, (uint16_t)cfg->viewport_width, (uint16_t)cfg->viewport_height);
    bgfx_set_view_frame_buffer(id, fb);
    bgfx_set_view_transform(id, view.raw[0], proj.raw[0]);
    bgfx_set_view_clear(id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_mode(id, BGFX_VIEW_MODE_SEQUENTIAL);
    return id;
}

void sr_cloud_temporal_destroy(JceSceneRenderer *sr)
{
    SrCloudTemporal *p = sr->cloud_temporal;
    if (!p) return;
    for (unsigned i = 0; i < CLOUD_VIEWPORT_COUNT; ++i)
        for (unsigned j = 0; j < 2; ++j)
            cloud_buffer_destroy(&p->history[i].buffer[j]);
    const bgfx_uniform_handle_t uniforms[] = {p->control, p->origin, p->previous_vp,
        p->current_vp, p->previous_eye, p->wind, p->prev_color, p->prev_distance, p->resolved,
        p->scene_depth, p->depth_params, p->layer_wind};
    for (unsigned i = 0; i < sizeof(uniforms) / sizeof(uniforms[0]); ++i)
        if (BGFX_HANDLE_IS_VALID(uniforms[i])) bgfx_destroy_uniform(uniforms[i]);
    if (BGFX_HANDLE_IS_VALID(p->program)) bgfx_destroy_program(p->program);
    if (BGFX_HANDLE_IS_VALID(p->composite)) bgfx_destroy_program(p->composite);
    JCE_FREE(p);
    sr->cloud_temporal = NULL;
}
