/* The full frame draws opaque, traces to a private target, then blends water
 * into the original framebuffer. The trace never binds scene color as an RTV. */
#include "jce_sr_internal.h"
#include "jce_water_optics_config.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "os/core/jce_memory.h"
#include <jce/renderer/jce_shaders.h>

enum { SR_WATER_OPTICS_TARGETS = 3 };

typedef struct {
    bgfx_frame_buffer_handle_t framebuffer;
    bgfx_texture_handle_t radiance;
    uint16_t width;
    uint16_t height;
} SrWaterOpticsTarget;

struct SrWaterOptics {
    SrWaterOpticsTarget targets[SR_WATER_OPTICS_TARGETS];
    SrWaterOpticsTarget *active;
    bgfx_program_handle_t trace;
    bgfx_program_handle_t resolve;
    bgfx_uniform_handle_t depth_sampler;
    bgfx_uniform_handle_t color_sampler;
    bgfx_uniform_handle_t radiance_sampler;
    bgfx_uniform_handle_t trace_params;
    bgfx_uniform_handle_t camera_params;
    bgfx_uniform_handle_t sky_params;
    bgfx_uniform_handle_t resolve_params;
    JceEntity body;
    bgfx_texture_handle_t opaque_color;
    bgfx_texture_handle_t opaque_depth;
    uint16_t trace_view;
    bool order_ready;
    bool sky_ready;
    bool usable;
};

typedef struct {
    JceEntity entity;
    unsigned count;
} SrWaterOpticsBody;

static void water_optics_find_body(JceScene *scene, JceEntity entity, void *user)
{
    SrWaterOpticsBody *body = user;
    if (body->count > 1u) return;
    const JceWaterComponent *water = jce_scene_get_water(scene, entity);
    const JceWaterOpticsComponent *optics = jce_scene_get_water_optics(scene, entity);
    if (!water || !water->visible || water->water_mode != JCE_WATER_MODE_FFT ||
        water->planar_reflection || water->size_x <= 0.0f || water->size_z <= 0.0f ||
        !jce_scene_has_transform(scene, entity) || !jce_water_optics_valid(optics) ||
        (optics->ssr_strength <= 0.0f && optics->refraction_strength <= 0.0f) ||
        !jce_scene_comp_enabled(scene, entity, jce_component_find("Water")) ||
        !jce_scene_comp_enabled(scene, entity, jce_component_find("WaterOptics")))
        return;
    body->entity = entity;
    ++body->count;
}

static JceEntity water_optics_single_body(JceScene *scene)
{
    SrWaterOpticsBody body = { JCE_ENTITY_INVALID, 0u };
    if (scene) jce_scene_each_water(scene, water_optics_find_body, &body);
    return body.count == 1u ? body.entity : JCE_ENTITY_INVALID;
}

static void water_optics_target_destroy(SrWaterOpticsTarget *target)
{
    if (BGFX_HANDLE_IS_VALID(target->framebuffer))
        bgfx_destroy_frame_buffer(target->framebuffer);
    if (BGFX_HANDLE_IS_VALID(target->radiance))
        bgfx_destroy_texture(target->radiance);
    target->framebuffer.idx = UINT16_MAX;
    target->radiance.idx = UINT16_MAX;
    target->width = target->height = 0;
}

static bool water_optics_target_ensure(SrWaterOpticsTarget *target,
                                       uint16_t width, uint16_t height)
{
    if (target->width == width && target->height == height)
        return BGFX_HANDLE_IS_VALID(target->framebuffer);

    const uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    if (!bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, flags))
        return false;
    bgfx_texture_handle_t radiance = bgfx_create_texture_2d(
        width, height, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, flags, NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(radiance)) return false;
    bgfx_frame_buffer_handle_t fb = bgfx_create_frame_buffer_from_handles(1, &radiance, false);
    if (!BGFX_HANDLE_IS_VALID(fb)) {
        bgfx_destroy_texture(radiance);
        return false;
    }

    water_optics_target_destroy(target);
    target->framebuffer = fb;
    target->radiance = radiance;
    target->width = width;
    target->height = height;
    return true;
}

static SrWaterOptics *water_optics_create(const JcePakArchive *pak)
{
    SrWaterOptics *optics = JCE_CALLOC(1, sizeof(*optics));
    if (!optics) return NULL;
    for (unsigned i = 0; i < SR_WATER_OPTICS_TARGETS; ++i) {
        optics->targets[i].framebuffer.idx = UINT16_MAX;
        optics->targets[i].radiance.idx = UINT16_MAX;
    }
    optics->trace.idx = jce_renderer_shader_load_named_internal(pak, "water", "water_optics_trace").idx;
    optics->resolve.idx = jce_renderer_shader_load_named_internal(pak, "water", "water_optics").idx;
    optics->depth_sampler = bgfx_create_uniform("s_water_optics_depth", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    optics->color_sampler = bgfx_create_uniform("s_water_optics_opaque", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    optics->radiance_sampler = bgfx_create_uniform("s_water_optics_resolved", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    optics->trace_params = bgfx_create_uniform("u_water_optics0", BGFX_UNIFORM_TYPE_VEC4, 1);
    optics->camera_params = bgfx_create_uniform("u_water_optics1", BGFX_UNIFORM_TYPE_VEC4, 1);
    optics->sky_params = bgfx_create_uniform("u_water_optics_sky", BGFX_UNIFORM_TYPE_VEC4, 1);
    optics->resolve_params = bgfx_create_uniform("u_water_optics_resolve", BGFX_UNIFORM_TYPE_VEC4, 1);
    optics->usable = BGFX_HANDLE_IS_VALID(optics->trace) &&
        BGFX_HANDLE_IS_VALID(optics->resolve) && BGFX_HANDLE_IS_VALID(optics->depth_sampler) &&
        BGFX_HANDLE_IS_VALID(optics->color_sampler) && BGFX_HANDLE_IS_VALID(optics->radiance_sampler) &&
        BGFX_HANDLE_IS_VALID(optics->trace_params) && BGFX_HANDLE_IS_VALID(optics->camera_params) &&
        BGFX_HANDLE_IS_VALID(optics->resolve_params) && BGFX_HANDLE_IS_VALID(optics->sky_params);
    return optics;
}

void sr_water_optics_destroy(SrWaterOptics *optics)
{
    if (!optics) return;
    for (unsigned i = 0; i < SR_WATER_OPTICS_TARGETS; ++i)
        water_optics_target_destroy(&optics->targets[i]);
    const bgfx_uniform_handle_t uniforms[] = {
        optics->depth_sampler, optics->color_sampler, optics->radiance_sampler,
        optics->trace_params, optics->camera_params, optics->resolve_params, optics->sky_params
    };
    for (unsigned i = 0; i < sizeof(uniforms) / sizeof(uniforms[0]); ++i)
        if (BGFX_HANDLE_IS_VALID(uniforms[i])) bgfx_destroy_uniform(uniforms[i]);
    if (BGFX_HANDLE_IS_VALID(optics->trace)) bgfx_destroy_program(optics->trace);
    if (BGFX_HANDLE_IS_VALID(optics->resolve)) bgfx_destroy_program(optics->resolve);
    JCE_FREE(optics);
}

static int water_optics_target_index(uint16_t base)
{
    switch (base) {
    case JCE_VIEW_EDITOR_SCENE: return 0;
    case JCE_VIEW_EDITOR_GAME: return 1;
    case JCE_VIEW_RUNTIME_GAME: return 2;
    default: return -1;
    }
}

bool sr_water_optics_prepare(JceSceneRenderer *sr, JceScene *scene,
                            const JceCamera *camera, uint16_t base,
                            const JceSceneRenderConfig *cfg)
{
    if (sr->water_optics) {
        sr->water_optics->order_ready = false;
        sr->water_optics->sky_ready = false;
        sr->water_optics->active = NULL;
    }
    if (cfg->reduced_pass || !cfg->draw_opaque || !cfg->draw_transparent ||
        cfg->view_mode != JCE_SCENE_VIEW_SHADED || sr->sky_legacy_gl21 ||
        !sr->pak || !camera ||
        jce_camera_get_mode(camera) != JCE_CAMERA_PERSPECTIVE ||
        cfg->scene_frame_buffer == UINT16_MAX || cfg->ssr_color_tex_handle == UINT16_MAX ||
        cfg->scene_depth_tex_handle == UINT16_MAX ||
        cfg->viewport_width == 0u || cfg->viewport_width > UINT16_MAX ||
        cfg->viewport_height == 0u || cfg->viewport_height > UINT16_MAX ||
        sr->taa_want_velocity ||
        (base == JCE_VIEW_EDITOR_SCENE && sr->cv_upscaler &&
         jce_cvar_get_int(sr->cv_upscaler) == 2))
        return false;

    const int index = water_optics_target_index(base);
    if (index < 0) return false;
    const JceEntity body = water_optics_single_body(scene);
    if (body == JCE_ENTITY_INVALID) return false;

    const JceSceneRenderingSettings *settings = jce_scene_get_rendering_settings(scene);
    const bool tonemap = sr->postfx_pipeline && !getenv("JCE_NO_POSTFX") &&
        (settings ? settings->postfx_enabled[JCE_POSTFX_TONEMAP] :
                    jce_postfx_is_enabled(sr->postfx_pipeline, JCE_POSTFX_TONEMAP));
    if (!sr->force_linear_output && !tonemap) return false;

    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || caps->limits.maxTextureSamplers < 16 ||
        cfg->viewport_width > caps->limits.maxTextureSize ||
        cfg->viewport_height > caps->limits.maxTextureSize ||
        !(caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER))
        return false;
    const bgfx_frame_buffer_handle_t scene_fb = { cfg->scene_frame_buffer };
    const bgfx_texture_handle_t color = bgfx_get_texture(scene_fb, 0);
    const bgfx_texture_handle_t depth = bgfx_get_texture(scene_fb, 1);
    if (color.idx != cfg->ssr_color_tex_handle || depth.idx != cfg->scene_depth_tex_handle)
        return false;

    if (!sr->water_optics) sr->water_optics = water_optics_create(sr->pak);
    SrWaterOptics *optics = sr->water_optics;
    if (!optics || !optics->usable) return false;
    optics->body = body;
    optics->opaque_color = color;
    optics->opaque_depth = depth;
    return true;
}

void sr_water_optics_order_ready(SrWaterOptics *optics, bool ready)
{
    if (optics) optics->order_ready = ready;
}

void sr_water_optics_sky_ready(SrWaterOptics *optics, bool ready)
{
    if (optics) optics->sky_ready = ready;
}

static void water_optics_set_view(uint16_t id, const char *name,
                                  bgfx_frame_buffer_handle_t fb,
                                  const jce_mat4 *view, const jce_mat4 *proj,
                                  uint16_t width, uint16_t height, uint16_t clear)
{
    bgfx_set_view_name(id, name, INT32_MAX);
    bgfx_set_view_frame_buffer(id, fb);
    bgfx_set_view_rect(id, 0, 0, width, height);
    bgfx_set_view_transform(id, JCE_M4_PTR(*view), JCE_M4_PTR(*proj));
    bgfx_set_view_mode(id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_set_view_clear(id, clear, 0, 1.0f, 0);
}

uint16_t sr_water_optics_begin(JceSceneRenderer *sr, const JceCamera *camera,
                               uint16_t base, const JceSceneRenderConfig *cfg)
{
    SrWaterOptics *optics = sr->water_optics;
    if (!optics || !optics->order_ready || !sr->postfx_tonemap_active)
        return base;

    const int index = water_optics_target_index(base);
    if (index < 0) return base;
    const uint16_t width = (uint16_t)cfg->viewport_width;
    const uint16_t height = (uint16_t)cfg->viewport_height;
    SrWaterOpticsTarget *target = &optics->targets[index];
    if (!water_optics_target_ensure(target, width, height)) return base;

    const uint16_t trace = (uint16_t)(base + JCE_VIEW_SR_WATER_TRACE_OFFSET);
    const uint16_t transparent = (uint16_t)(base + JCE_VIEW_SR_TRANSPARENCY_OFFSET);
    const jce_mat4 view = jce_camera_view(camera);
    const jce_mat4 proj = jce_camera_proj(camera, (float)width / (float)height,
                                          sr->homogeneous_depth);
    const bgfx_frame_buffer_handle_t scene_fb = { cfg->scene_frame_buffer };
    water_optics_set_view(trace, "Scene/WaterOpticsTrace", target->framebuffer,
                          &view, &proj, width, height, BGFX_CLEAR_COLOR);
    water_optics_set_view(transparent, "Scene/Transparent", scene_fb,
                          &view, &proj, width, height, BGFX_CLEAR_NONE);
    optics->trace_view = trace;
    optics->active = target;
    return transparent;
}

/* The caller has set the exact water mesh, vertex textures and uniforms. The
 * trace changes two sampler stages and draw state, which the caller restores
 * before the resolved water draw. */
bgfx_program_handle_t sr_water_optics_trace(JceSceneRenderer *sr, JceScene *scene,
                                             const JceCamera *camera, JceEntity entity)
{
    const bgfx_program_handle_t invalid = { UINT16_MAX };
    SrWaterOptics *optics = sr->water_optics;
    if (!optics || !optics->active || entity != optics->body) return invalid;
    const JceWaterOpticsComponent *component = jce_scene_get_water_optics(scene, entity);
    if (!component) return invalid;

    const float trace_params[4] = { component->ssr_strength, component->refraction_strength,
                                    1.333f, 20.0f };
    const float camera_params[4] = { jce_camera_get_near(camera), jce_camera_get_far(camera),
                                     0.18f, 16.0f };
    const float sky_params[4] = { optics->sky_ready ? 1.0f : 0.0f,
        1.0f / (float)optics->active->width, 1.0f / (float)optics->active->height, 0.0f };
    const uint32_t clamp_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bgfx_set_texture(2, optics->depth_sampler, optics->opaque_depth,
                     clamp_flags | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    bgfx_set_texture(3, optics->color_sampler, optics->opaque_color, clamp_flags);
    bgfx_set_uniform(optics->trace_params, trace_params, 1);
    bgfx_set_uniform(optics->camera_params, camera_params, 1);
    bgfx_set_uniform(optics->sky_params, sky_params, 1);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(optics->trace_view, optics->trace, 0, BGFX_DISCARD_NONE);

    const float resolve[4] = { 1.0f / (float)optics->active->width,
                               1.0f / (float)optics->active->height, 0.0f, 0.0f };
    bgfx_set_texture(15, optics->radiance_sampler, optics->active->radiance,
                     clamp_flags | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    bgfx_set_uniform(optics->resolve_params, resolve, 1);
    return optics->resolve;
}
