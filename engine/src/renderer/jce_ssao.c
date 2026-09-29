/*
 * jce_ssao.c -- screen-space ambient occlusion implementation.
 *
 * Two passes:
 *   1. Sampling pass: full-screen quad reads depth, reconstructs
 *      normal from depth derivatives, samples 16 hemisphere offsets,
 *      accumulates occlusion. Output: R8 framebuffer (use RGBA8 for
 *      simplicity).
 *   2. Blur pass: 5x5 box blur of (1).
 */

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_ssao.h>
#include <jce/renderer/jce_views.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_fullscreen_pass.h"
#include "renderer/jce_shader_load.h"   /* backend suffix + engine-pak fallback */

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "ssao"


struct JceSsao {
    JceSsaoParams params;

    bgfx_program_handle_t       prog_sample;
    bgfx_program_handle_t       prog_blur;

    bgfx_uniform_handle_t       u_p0;       /* radius/bias/intensity/scale */
    bgfx_uniform_handle_t       u_p1;       /* near/far */
    bgfx_uniform_handle_t       u_screen;
    bgfx_uniform_handle_t       u_kernel;   /* 16 vec4 */
    bgfx_uniform_handle_t       u_cs_sun;   /* contact shadow: sun + steps */
    bgfx_uniform_handle_t       u_cs_p;     /* contact shadow: proj/ray/jitter */
    bgfx_uniform_handle_t       u_cloud;    /* cloud map extent/centre/strength */
    bgfx_uniform_handle_t       s_cloud;
    bgfx_uniform_handle_t       s_depth;
    bgfx_uniform_handle_t       s_ao;

    /* Shared with SSR, volumetric fog and SSGI: jce_fullscreen_pass.h.
     * TWO targets here -- the raw AO and its blur -- which is why the helper
     * models one target rather than owning the module's whole set. */
    JceFsQuad   quad;
    JceFsTarget raw;
    JceFsTarget blur;
};

JceSsaoParams jce_ssao_default_params(void)
{
    /* Zero FIRST, then set what has a non-zero default.  Field-by-field
     * assignment leaves every field added later as uninitialised stack, and a
     * garbage cs_steps would march the contact-shadow ray with whatever the
     * previous call frame happened to leave there -- a defect that appears and
     * disappears with unrelated code changes. */
    JceSsaoParams p;
    memset(&p, 0, sizeof p);
    p.radius     = 1.0f;
    p.bias       = 0.005f;
    p.intensity  = 1.5f;
    p.near_plane = 0.1f;
    p.far_plane  = 1000.0f;
    return p;
}

JceSsao *jce_ssao_create(const JceSsaoDesc *desc)
{
    if (!desc || !desc->pak) return NULL;
    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) return NULL;

    JceSsao *s = (JceSsao *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    const int want_w = desc->width  > 0 ? desc->width  : 1280;
    const int want_h = desc->height > 0 ? desc->height : 720;
    s->params = jce_ssao_default_params();
    jce_fs_target_init(&s->raw);
    jce_fs_target_init(&s->blur);
    jce_fs_quad_init(&s->quad);


    bgfx_shader_handle_t vsh   = jce_shader_load_from_pak(desc->pak, "vs_ssao",      sfx, LOG_TAG);
    bgfx_shader_handle_t fsh_a = jce_shader_load_from_pak(desc->pak, "fs_ssao",      sfx, LOG_TAG);
    bgfx_shader_handle_t fsh_b = jce_shader_load_from_pak(desc->pak, "fs_ssao_blur", sfx, LOG_TAG);
    if (vsh.idx == UINT16_MAX || fsh_a.idx == UINT16_MAX || fsh_b.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed");
        if (vsh.idx   != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh_a.idx != UINT16_MAX) bgfx_destroy_shader(fsh_a);
        if (fsh_b.idx != UINT16_MAX) bgfx_destroy_shader(fsh_b);
        jce_fs_quad_destroy(&s->quad);
        JCE_FREE(s);
        return NULL;
    }
    s->prog_sample = bgfx_create_program(vsh,   fsh_a, false);
    s->prog_blur   = bgfx_create_program(vsh,   fsh_b, true);
    bgfx_destroy_shader(vsh);

    s->u_p0     = bgfx_create_uniform("u_ssao_params0", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_p1     = bgfx_create_uniform("u_ssao_params1", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_cs_sun = bgfx_create_uniform("u_cs_sun_view",  BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_cs_p   = bgfx_create_uniform("u_cs_params",    BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_cloud  = bgfx_create_uniform("u_ssao_cloud",   BGFX_UNIFORM_TYPE_VEC4, 1);
    s->s_cloud  = bgfx_create_uniform("s_ssao_cloud",   BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->u_screen = bgfx_create_uniform("u_screen",       BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_kernel = bgfx_create_uniform("u_kernel",       BGFX_UNIFORM_TYPE_VEC4, 16);
    s->s_depth  = bgfx_create_uniform("s_depth",        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_ao     = bgfx_create_uniform("s_ao",           BGFX_UNIFORM_TYPE_SAMPLER, 1);

    jce_fs_target_create(&s->raw, want_w, want_h, BGFX_TEXTURE_FORMAT_RGBA8,
                         BGFX_SAMPLER_POINT | BGFX_SAMPLER_U_CLAMP
                           | BGFX_SAMPLER_V_CLAMP);
    jce_fs_target_create(&s->blur, want_w, want_h, BGFX_TEXTURE_FORMAT_RGBA8,
                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    return s;
}

void jce_ssao_destroy(JceSsao *s)
{
    if (!s) return;
    if (s->prog_sample.idx != UINT16_MAX) bgfx_destroy_program(s->prog_sample);
    if (s->prog_blur.idx   != UINT16_MAX) bgfx_destroy_program(s->prog_blur);
    if (s->u_p0.idx     != UINT16_MAX) bgfx_destroy_uniform(s->u_p0);
    if (s->u_p1.idx     != UINT16_MAX) bgfx_destroy_uniform(s->u_p1);
    if (s->u_screen.idx != UINT16_MAX) bgfx_destroy_uniform(s->u_screen);
    if (s->u_kernel.idx != UINT16_MAX) bgfx_destroy_uniform(s->u_kernel);
    if (s->u_cs_sun.idx != UINT16_MAX) bgfx_destroy_uniform(s->u_cs_sun);
    if (s->u_cs_p.idx   != UINT16_MAX) bgfx_destroy_uniform(s->u_cs_p);
    if (s->u_cloud.idx  != UINT16_MAX) bgfx_destroy_uniform(s->u_cloud);
    if (s->s_cloud.idx  != UINT16_MAX) bgfx_destroy_uniform(s->s_cloud);
    if (s->s_depth.idx  != UINT16_MAX) bgfx_destroy_uniform(s->s_depth);
    if (s->s_ao.idx     != UINT16_MAX) bgfx_destroy_uniform(s->s_ao);
    jce_fs_quad_destroy(&s->quad);
    jce_fs_target_destroy(&s->raw);
    jce_fs_target_destroy(&s->blur);
    JCE_FREE(s);
}

void jce_ssao_resize(JceSsao *s, int w, int h)
{
    if (!s || w <= 0 || h <= 0) return;
    jce_fs_target_resize(&s->raw,  w, h);
    jce_fs_target_resize(&s->blur, w, h);
}

void jce_ssao_set_params(JceSsao *s, const JceSsaoParams *p)
{
    if (!s || !p) return;
    s->params = *p;
}

static void build_kernel(float out[16][4])
{
    /* Pre-baked Halton-2,3 hemisphere samples. */
    static const float seeds[16][3] = {
        { 0.50f,  0.10f, 0.20f}, {-0.30f,  0.50f, 0.30f}, { 0.10f, -0.40f, 0.40f},
        {-0.20f, -0.30f, 0.50f}, { 0.40f,  0.20f, 0.10f}, {-0.10f,  0.30f, 0.60f},
        { 0.30f, -0.20f, 0.30f}, {-0.40f, -0.10f, 0.20f}, { 0.20f,  0.40f, 0.40f},
        {-0.50f,  0.20f, 0.10f}, { 0.10f,  0.10f, 0.70f}, {-0.20f, -0.50f, 0.30f},
        { 0.40f, -0.30f, 0.20f}, {-0.10f,  0.40f, 0.50f}, { 0.30f,  0.30f, 0.20f},
        {-0.30f, -0.40f, 0.40f},
    };
    for (int i = 0; i < 16; i++) {
        float s = (float)(i + 1) / 16.0f;
        s = 0.1f + s * s * 0.9f;  /* concentrate near origin */
        out[i][0] = seeds[i][0] * s;
        out[i][1] = seeds[i][1] * s;
        out[i][2] = seeds[i][2] * s;
        out[i][3] = 0.0f;
    }
}

void jce_ssao_render(JceSsao *s, uint16_t depth_tex_handle,
                     const jce_mat4 *view, const jce_mat4 *proj,
                     uint16_t sample_view_id, uint16_t blur_view_id)
{
    if (!s) return;
    if (s->prog_sample.idx == UINT16_MAX) return;

    JCE_PROFILE_ZONE_N("SSAO::Render");

    /* Both named by the caller.  This used to derive the blur view as
     * first_view_id + 1, so the second id was invisible at every call site and
     * an engine-private comment declared it free -- see jce_ssao.h. */
    uint16_t v_sample = sample_view_id;
    uint16_t v_blur   = blur_view_id;

    /* ---- pass 1: sampling ---- */
    bgfx_set_view_frame_buffer(v_sample, s->raw.fb);
    bgfx_set_view_rect(v_sample, 0, 0, (uint16_t)s->raw.w, (uint16_t)s->raw.h);
    bgfx_set_view_clear(v_sample, BGFX_CLEAR_COLOR, 0xFFFFFFFF, 1.0f, 0);
    /* Publishes u_invViewProj for the cloud-shadow lookup, which is a WORLD
     * space map: without the inverse there is no route from a depth sample
     * back to a world XZ.  Identity when the caller has no matrices, which
     * disables the lookup rather than reconstructing garbage positions. */
    {
        const jce_mat4 vi = view ? *view : jce_m4_identity();
        const jce_mat4 pi = proj ? *proj : jce_m4_identity();
        bgfx_set_view_transform(v_sample, JCE_M4_PTR(vi), JCE_M4_PTR(pi));
    }
    bgfx_touch(v_sample);

    float p0[4] = { s->params.radius, s->params.bias, s->params.intensity, 1.0f };
    float p1[4] = { s->params.near_plane, s->params.far_plane, 0.0f, 0.0f };
    float screen[4] = { 1.0f / (float)s->raw.w, 1.0f / (float)s->raw.h,
                        (float)s->raw.w, (float)s->raw.h };
    float kernel[16][4];
    build_kernel(kernel);

    bgfx_set_uniform(s->u_p0, p0, 1);
    bgfx_set_uniform(s->u_p1, p1, 1);

    /* Contact-shadow march.  Bound UNCONDITIONALLY: an unset uniform keeps
     * whatever the previous submit left in it, so a frame that skipped the
     * write would march with a stale sun -- shadows that lag the light by an
     * arbitrary amount and only on some frames, which is far harder to
     * recognise than shadows that are simply absent. */
    {
        const float steps = (s->params.cs_ray_length > 0.0f)
                          ? s->params.cs_steps : 0.0f;
        float sun[4] = { s->params.cs_sun_view[0], s->params.cs_sun_view[1],
                         s->params.cs_sun_view[2], steps };
        float csp[4] = { s->params.cs_proj_scale[0], s->params.cs_proj_scale[1],
                         s->params.cs_ray_length, s->params.cs_jitter };
        bgfx_set_uniform(s->u_cs_sun, sun, 1);
        bgfx_set_uniform(s->u_cs_p,   csp, 1);
    }

    /* Cloud shadows.  Bound unconditionally for the same reason as above: a
     * retained uniform from a frame that had a map would keep shadowing the
     * world after the map went away. */
    {
        bgfx_texture_handle_t ct = { s->params.cloud_tex };
        const bool have = BGFX_HANDLE_IS_VALID(ct)
                       && s->params.cloud_extent > 0.0f
                       && (view != NULL) && (proj != NULL);
        float c[4] = {
            have ? s->params.cloud_extent : 0.0f,
            s->params.cloud_center[0],
            s->params.cloud_center[1],
            have ? s->params.cloud_strength : 0.0f
        };
        bgfx_set_uniform(s->u_cloud, c, 1);
        /* A neutral bind when there is no map: an unbound sampler is
         * undefined on several backends, and undefined here means the world is
         * shadowed by garbage. */
        bgfx_texture_handle_t dep = { depth_tex_handle };
        bgfx_texture_handle_t bind = have ? ct : dep;
        bgfx_set_texture(1, s->s_cloud, bind,
                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    }
    bgfx_set_uniform(s->u_screen, screen, 1);
    bgfx_set_uniform(s->u_kernel, kernel, 16);

    bgfx_texture_handle_t depth = { depth_tex_handle };
    bgfx_set_texture(0, s->s_depth, depth, UINT32_MAX);
    jce_fs_quad_bind(&s->quad);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(v_sample, s->prog_sample, 0, BGFX_DISCARD_ALL);

    /* ---- pass 2: blur ---- */
    bgfx_set_view_frame_buffer(v_blur, s->blur.fb);
    bgfx_set_view_rect(v_blur, 0, 0, (uint16_t)s->raw.w, (uint16_t)s->raw.h);
    bgfx_set_view_clear(v_blur, BGFX_CLEAR_COLOR, 0xFFFFFFFF, 1.0f, 0);
    bgfx_touch(v_blur);

    bgfx_set_uniform(s->u_screen, screen, 1);
    bgfx_set_texture(0, s->s_ao, s->raw.tex, UINT32_MAX);
    jce_fs_quad_bind(&s->quad);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(v_blur, s->prog_blur, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

uint16_t jce_ssao_get_result_texture(const JceSsao *s)
{
    if (!s) return UINT16_MAX;
    return s->blur.tex.idx;
}
