/*
 * jce_volumetric_fog.c -- analytic + raymarched homogeneous volumetric fog.
 *
 * Single full-screen pass. For each pixel: reconstruct world-space ray
 * from camera through the pixel, march N samples up to the scene depth,
 * accumulate transmittance and in-scatter using Beer-Lambert.
 */

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_views.h>
#include "renderer/jce_volumetric_fog_internal.h"

#include "os/core/jce_memory.h"
#include "renderer/jce_fullscreen_pass.h"
#include "renderer/jce_shader_load.h"   /* backend suffix + engine-pak fallback */

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "volfog"

typedef struct { float pos[2]; float uv[2]; } VfQuadV;

typedef struct JceFogScreenShafts {
    JceFsTarget target;
    bgfx_program_handle_t radial;
    bgfx_program_handle_t composite;
    bgfx_uniform_handle_t sampler;
    bgfx_uniform_handle_t source;
    bgfx_uniform_handle_t tint;
    float radiance[4];
} JceFogScreenShafts;

struct JceVolumetricFog {
    JceFogScreenShafts *screen_shafts;
    bool screen_shafts_attempted;
    bool screen_shafts_rendered;
    JceVolumetricFogParams params;
    float noise[4];
    float lobes[4];
    float volume[4];

    bgfx_program_handle_t  prog;

    bgfx_uniform_handle_t  u_p0;
    bgfx_uniform_handle_t  u_p1;
    bgfx_uniform_handle_t  u_color;
    bgfx_uniform_handle_t  u_noise;
    bgfx_uniform_handle_t  u_lobes;
    bgfx_uniform_handle_t  u_volume;
    bgfx_uniform_handle_t  u_cloud_map;
    bgfx_uniform_handle_t  u_cloud_ray;
    bgfx_uniform_handle_t  s_cloud_shadow;
    bgfx_uniform_handle_t  u_sun;        /* dir.xyz + Henyey-Greenstein g */
    bgfx_uniform_handle_t  u_sun_color;  /* rgb + cascade count           */
    bgfx_uniform_handle_t  u_csm_vp;     /* mat4[4]                       */
    bgfx_uniform_handle_t  u_csm_splits;
    bgfx_uniform_handle_t  u_csm_params;
    bgfx_uniform_handle_t  s_csm[4];
    bgfx_uniform_handle_t  s_depth;

    /* Composite pass — draws the fog RT into a destination frame
     * buffer with blend ONE/SRC_ALPHA. */
    bgfx_program_handle_t  prog_composite;
    bgfx_uniform_handle_t  s_fog;
    bool                   composite_ok;

    /* Shared with SSAO, SSR and SSGI: jce_fullscreen_pass.h. */
    JceFsQuad   quad;
    JceFsTarget rt;
};

JceVolumetricFogParams jce_volumetric_fog_default_params(void)
{
    JceVolumetricFogParams p;
    p.density        = 0.02f;
    p.scattering     = 1.0f;
    p.height_falloff = 0.05f;
    p.height_origin  = 0.0f;
    p.max_distance   = 800.0f;
    p.step_count     = 32.0f;
    p.near_plane     = 0.1f;
    p.far_plane      = 1000.0f;
    p.color_r        = 0.65f;
    p.color_g        = 0.72f;
    p.color_b        = 0.80f;
    p.ambient_lift   = 0.05f;
    return p;
}

JceVolumetricFog *jce_volumetric_fog_create(const JceVolumetricFogDesc *desc)
{
    if (!desc || !desc->pak) return NULL;
    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) return NULL;

    JceVolumetricFog *f = (JceVolumetricFog *)JCE_CALLOC(1, sizeof(*f));
    if (!f) return NULL;
    const int want_w = desc->width  > 0 ? desc->width  : 1280;
    const int want_h = desc->height > 0 ? desc->height : 720;
    f->params = jce_volumetric_fog_default_params();
    jce_volumetric_fog_set_look(f, NULL, NULL);
    jce_fs_target_init(&f->rt);
    jce_fs_quad_init(&f->quad);


    bgfx_shader_handle_t vsh = jce_shader_load_from_pak(desc->pak, "vs_volfog", sfx, LOG_TAG);
    bgfx_shader_handle_t fsh = jce_shader_load_from_pak(desc->pak, "fs_volfog", sfx, LOG_TAG);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed");
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        jce_fs_quad_destroy(&f->quad);
        JCE_FREE(f);
        return NULL;
    }
    f->prog = bgfx_create_program(vsh, fsh, true);

    f->u_p0    = bgfx_create_uniform("u_volfog_p0",    BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_p1    = bgfx_create_uniform("u_volfog_p1",    BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_color = bgfx_create_uniform("u_volfog_color", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_noise = bgfx_create_uniform("u_volfog_noise", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_lobes = bgfx_create_uniform("u_volfog_lobes", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_volume = bgfx_create_uniform("u_volfog_volume", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_cloud_map = bgfx_create_uniform("u_volfog_cloud_map", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_cloud_ray = bgfx_create_uniform("u_volfog_cloud_ray", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->s_cloud_shadow = bgfx_create_uniform("s_volfog_cloud_shadow", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    f->u_sun        = bgfx_create_uniform("u_volfog_sun",        BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_sun_color  = bgfx_create_uniform("u_volfog_sun_color",  BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_csm_vp     = bgfx_create_uniform("u_volfog_csm_vp",     BGFX_UNIFORM_TYPE_MAT4, 4);
    f->u_csm_splits = bgfx_create_uniform("u_volfog_csm_splits", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_csm_params = bgfx_create_uniform("u_volfog_csm_params", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->s_csm[0] = bgfx_create_uniform("s_volfog_csm0", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    f->s_csm[1] = bgfx_create_uniform("s_volfog_csm1", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    f->s_csm[2] = bgfx_create_uniform("s_volfog_csm2", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    f->s_csm[3] = bgfx_create_uniform("s_volfog_csm3", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    f->s_depth = bgfx_create_uniform("s_depth",        BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Composite pass program (vs_volfog reused). Optional — if the
     * shader binary is missing on this backend the renderer still
     * renders fog into its private RT and callers can fall back to
     * jce_scene_renderer_get_fog_result_texture(). */
    f->prog_composite.idx = UINT16_MAX;
    f->s_fog.idx          = UINT16_MAX;
    f->composite_ok       = false;
    {
        bgfx_shader_handle_t cvsh = jce_shader_load_from_pak(desc->pak, "vs_volfog", sfx, LOG_TAG);
        bgfx_shader_handle_t cfsh = jce_shader_load_from_pak(desc->pak, "fs_volfog_composite", sfx,
                                                            LOG_TAG);
        if (cvsh.idx != UINT16_MAX && cfsh.idx != UINT16_MAX) {
            f->prog_composite = bgfx_create_program(cvsh, cfsh, true);
            f->s_fog          = bgfx_create_uniform("s_fog",
                                                    BGFX_UNIFORM_TYPE_SAMPLER, 1);
            f->composite_ok   = (f->prog_composite.idx != UINT16_MAX);
        } else {
            if (cvsh.idx != UINT16_MAX) bgfx_destroy_shader(cvsh);
            if (cfsh.idx != UINT16_MAX) bgfx_destroy_shader(cfsh);
            LOG_INFO(LOG_TAG, "composite shader unavailable; render-only");
        }
    }

    jce_fs_target_create(&f->rt, want_w, want_h, BGFX_TEXTURE_FORMAT_RGBA8,
                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    return f;
}

static void vfog_screen_shafts_destroy(JceFogScreenShafts *s)
{
    if (!s) return;
    if (BGFX_HANDLE_IS_VALID(s->radial)) bgfx_destroy_program(s->radial);
    if (BGFX_HANDLE_IS_VALID(s->composite)) bgfx_destroy_program(s->composite);
    if (BGFX_HANDLE_IS_VALID(s->sampler)) bgfx_destroy_uniform(s->sampler);
    if (BGFX_HANDLE_IS_VALID(s->source)) bgfx_destroy_uniform(s->source);
    if (BGFX_HANDLE_IS_VALID(s->tint)) bgfx_destroy_uniform(s->tint);
    jce_fs_target_destroy(&s->target);
    JCE_FREE(s);
}

void jce_volumetric_fog_destroy(JceVolumetricFog *f)
{
    if (!f) return;
    vfog_screen_shafts_destroy(f->screen_shafts);
    if (f->prog.idx           != UINT16_MAX) bgfx_destroy_program(f->prog);
    if (f->prog_composite.idx != UINT16_MAX) bgfx_destroy_program(f->prog_composite);
    if (f->u_p0.idx           != UINT16_MAX) bgfx_destroy_uniform(f->u_p0);
    if (f->u_p1.idx           != UINT16_MAX) bgfx_destroy_uniform(f->u_p1);
    if (f->u_color.idx        != UINT16_MAX) bgfx_destroy_uniform(f->u_color);
    if (f->u_noise.idx        != UINT16_MAX) bgfx_destroy_uniform(f->u_noise);
    if (f->u_lobes.idx        != UINT16_MAX) bgfx_destroy_uniform(f->u_lobes);
    if (BGFX_HANDLE_IS_VALID(f->u_volume)) bgfx_destroy_uniform(f->u_volume);
    if (BGFX_HANDLE_IS_VALID(f->u_cloud_map)) bgfx_destroy_uniform(f->u_cloud_map);
    if (BGFX_HANDLE_IS_VALID(f->u_cloud_ray)) bgfx_destroy_uniform(f->u_cloud_ray);
    if (BGFX_HANDLE_IS_VALID(f->s_cloud_shadow)) bgfx_destroy_uniform(f->s_cloud_shadow);
    if (f->u_sun.idx        != UINT16_MAX) bgfx_destroy_uniform(f->u_sun);
    if (f->u_sun_color.idx  != UINT16_MAX) bgfx_destroy_uniform(f->u_sun_color);
    if (f->u_csm_vp.idx     != UINT16_MAX) bgfx_destroy_uniform(f->u_csm_vp);
    if (f->u_csm_splits.idx != UINT16_MAX) bgfx_destroy_uniform(f->u_csm_splits);
    if (f->u_csm_params.idx != UINT16_MAX) bgfx_destroy_uniform(f->u_csm_params);
    for (int i = 0; i < 4; i++)
        if (f->s_csm[i].idx != UINT16_MAX) bgfx_destroy_uniform(f->s_csm[i]);
    if (f->s_depth.idx        != UINT16_MAX) bgfx_destroy_uniform(f->s_depth);
    if (f->s_fog.idx          != UINT16_MAX) bgfx_destroy_uniform(f->s_fog);
    jce_fs_quad_destroy(&f->quad);
    jce_fs_target_destroy(&f->rt);
    JCE_FREE(f);
}

void jce_volumetric_fog_resize(JceVolumetricFog *f, int w, int h)
{
    if (!f || w <= 0 || h <= 0) return;
    jce_fs_target_resize(&f->rt, w, h);
}

void jce_volumetric_fog_set_params(JceVolumetricFog *f, const JceVolumetricFogParams *p)
{
    if (!f || !p) return;
    JceVolumetricFogParams q = *p;
    if (q.density        <  0.0f) q.density        = 0.0f;
    if (q.scattering     <  0.0f) q.scattering     = 0.0f;
    if (q.height_falloff <  0.0f) q.height_falloff = 0.0f;
    if (q.max_distance   <= 0.0f) q.max_distance   = 1.0f;
    q.step_count = isfinite(q.step_count)
        ? floorf(fminf(fmaxf(q.step_count, 1.0f), 64.0f) + 0.5f) : 32.0f;
    if (q.near_plane     <= 0.0f) q.near_plane     = 0.01f;
    if (q.far_plane      <= q.near_plane) q.far_plane = q.near_plane + 1.0f;
    if (q.color_r        <  0.0f) q.color_r        = 0.0f;
    if (q.color_g        <  0.0f) q.color_g        = 0.0f;
    if (q.color_b        <  0.0f) q.color_b        = 0.0f;
    f->params = q;
}

static float volfog_look_value(float value, float fallback, float lo, float hi)
{
    return isfinite(value) ? fminf(fmaxf(value, lo), hi) : fallback;
}

void jce_volumetric_fog_set_look(JceVolumetricFog *f,
    const JceVolumetricFogLook *look, const double wind_offset_xz_m[2])
{
    if (!f) return;

    memset(f->noise, 0, sizeof f->noise);
    memset(f->lobes, 0, sizeof f->lobes);
    memset(f->volume, 0, sizeof f->volume);
    if (!look) return;

    f->volume[3] = volfog_look_value(look->volume_radius_m, 0.0f, 0.0f, 100000.0f);
    for (int axis = 0; axis < 3; ++axis)
        f->volume[axis] = volfog_look_value(look->volume_center[axis], 0.0f, -100000.0f, 100000.0f);

    f->lobes[0] = volfog_look_value(look->forward_strength, 1.0f, 0.0f, 4.0f);
    f->lobes[1] = volfog_look_value(look->backward_strength, 0.0f, 0.0f, 4.0f);
    f->lobes[2] = (f->lobes[0] != 1.0f || f->lobes[1] > 0.0f) ? 1.0f : 0.0f;
    f->noise[0] = volfog_look_value(look->noise_strength, 0.0f, 0.0f, 1.0f);
    if (f->noise[0] == 0.0f) return;

    const float cell_m = volfog_look_value(look->noise_cell_m, 24.0f, 1.0f, 256.0f);
    f->noise[1] = 1.0f / cell_m;
    /* Match the shader lattice's 64-cell period, in its own units. */
    if (wind_offset_xz_m) {
        for (int axis = 0; axis < 2; ++axis)
            if (isfinite(wind_offset_xz_m[axis]))
                f->noise[2 + axis] = (float)fmod(wind_offset_xz_m[axis] / cell_m, 64.0);
    }
}

void jce_volumetric_fog_render(JceVolumetricFog *f,
                               uint16_t depth_tex_handle,
                               const jce_mat4 *view, const jce_mat4 *proj,
                               const JceVolumetricFogSun *sun,
                               uint16_t first_view_id)
{
    jce_volumetric_fog_render_with_cloud(f, depth_tex_handle, view, proj, sun,
                                         NULL, first_view_id);
}

void jce_volumetric_fog_render_with_cloud(JceVolumetricFog *f,
    uint16_t depth_tex_handle, const jce_mat4 *view, const jce_mat4 *proj,
    const JceVolumetricFogSun *sun, const JceVolumetricFogCloudShadow *cloud,
    uint16_t first_view_id)
{
    if (!f) return;
    f->screen_shafts_rendered = false;
    if (f->prog.idx == UINT16_MAX || !view || !proj || depth_tex_handle == UINT16_MAX)
        return;
    JCE_PROFILE_ZONE_N("Renderer::VolumetricFog::render");

    uint16_t v = first_view_id;
    bgfx_set_view_name(v, "Scene/VolumetricFog", INT32_MAX);
    bgfx_set_view_frame_buffer(v, f->rt.fb);
    bgfx_set_view_rect(v, 0, 0, (uint16_t)f->rt.w, (uint16_t)f->rt.h);
    bgfx_set_view_clear(v, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
    /* bgfx auto-fills u_view, u_proj, u_invViewProj from this. */
    bgfx_set_view_transform(v, JCE_M4_PTR(*view), JCE_M4_PTR(*proj));
    bgfx_touch(v);

    float p0[4] = { f->params.density, f->params.scattering,
                    f->params.near_plane, f->params.far_plane };
    float p1[4] = { f->params.step_count, f->params.height_falloff,
                    f->params.height_origin, f->params.max_distance };
    float col[4] = { f->params.color_r, f->params.color_g,
                     f->params.color_b, f->params.ambient_lift };

    bgfx_set_uniform(f->u_p0,    p0,  1);
    bgfx_set_uniform(f->u_p1,    p1,  1);
    bgfx_set_uniform(f->u_color, col, 1);
    if (BGFX_HANDLE_IS_VALID(f->u_noise)) bgfx_set_uniform(f->u_noise, f->noise, 1);
    if (BGFX_HANDLE_IS_VALID(f->u_lobes)) bgfx_set_uniform(f->u_lobes, f->lobes, 1);
    if (BGFX_HANDLE_IS_VALID(f->u_volume)) bgfx_set_uniform(f->u_volume, f->volume, 1);

    const bool have_cloud_bindings = BGFX_HANDLE_IS_VALID(f->u_cloud_map) &&
        BGFX_HANDLE_IS_VALID(f->u_cloud_ray) && BGFX_HANDLE_IS_VALID(f->s_cloud_shadow);
    const bool cloud_on = have_cloud_bindings && sun && sun->enabled &&
        sun->cascade_count > 0u && cloud &&
        cloud->texture != UINT16_MAX && isfinite(cloud->extent_m) && cloud->extent_m > 0.0f &&
        isfinite(cloud->center_xz[0]) && isfinite(cloud->center_xz[1]) &&
        isfinite(cloud->base_y_m) && isfinite(cloud->sun_slope_xz[0]) &&
        isfinite(cloud->sun_slope_xz[1]);
    if (have_cloud_bindings) {
        float map[4] = {0};
        float ray[4] = {0};
        bgfx_texture_handle_t tex = { depth_tex_handle };
        if (cloud_on) {
            tex.idx = cloud->texture;
            map[0] = cloud->extent_m;
            map[1] = cloud->center_xz[0];
            map[2] = cloud->center_xz[1];
            map[3] = cloud->base_y_m;
            ray[0] = cloud->sun_slope_xz[0];
            ray[1] = cloud->sun_slope_xz[1];
        }
        bgfx_set_uniform(f->u_cloud_map, map, 1);
        bgfx_set_uniform(f->u_cloud_ray, ray, 1);
        bgfx_set_texture(6, f->s_cloud_shadow, tex,
                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    }

    /* Sun + cascades.  Bound UNCONDITIONALLY, including the disabled case:
     * bgfx retains uniform values between submits, so a frame that skipped the
     * write would march against whatever the last frame that DID set them left
     * behind -- shafts pointing at a sun that has moved, on some frames only. */
    {
        const bool on = sun && sun->enabled && sun->cascade_count > 0u;
        float sd[4]  = { 0.0f, 1.0f, 0.0f, 0.0f };
        float sc[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
        float spl[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float par[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        jce_mat4 vps[4];
        for (int i = 0; i < 4; i++) vps[i] = jce_m4_identity();

        if (on) {
            sd[0] = sun->sun_dir[0]; sd[1] = sun->sun_dir[1];
            sd[2] = sun->sun_dir[2];
            /* Clamp g strictly inside (-1,1): the phase function divides by
             * (1+g^2-2g*cos)^1.5, which is exactly 0 at g=1 looking straight
             * at the sun.  A value of 1 from a config file would paint NaN. */
            float g = sun->anisotropy;
            if (g >  0.95f) g =  0.95f;
            if (g < -0.95f) g = -0.95f;
            sd[3] = g;

            sc[0] = sun->sun_color[0]; sc[1] = sun->sun_color[1];
            sc[2] = sun->sun_color[2];
            sc[3] = (float)((sun->cascade_count > 4u) ? 4u : sun->cascade_count);

            for (uint32_t i = 0; i < 4u; i++) {
                if (i < sun->cascade_count) {
                    vps[i] = sun->cascade_vp[i];
                    spl[i] = sun->splits[i];
                }
            }
            /* Pad the unused split lanes with the last real one.
             *
             * The shader reads them as an ordered ladder and reads lane 3 as
             * the shadow reach for its distance fade, exactly as
             * csm_shadow.sh does -- so a zero there does not mean "no
             * cascade", it means "the ladder ends at zero", which sends every
             * sample past the last real split into cascade 3 and disables the
             * fade. Same defect the static path carried; fixed there in
             * sr_pack_csm_splits and duplicated here rather than shared,
             * because this struct is the renderer's public one and the other
             * is internal. */
            for (uint32_t i = sun->cascade_count; i < 4u; i++)
                spl[i] = (sun->cascade_count > 0u)
                       ? sun->splits[sun->cascade_count - 1u] : 0.0f;
            par[0] = sun->inv_map_size;
            /* The blend fraction the SURFACE path uses for the same splits.
             * Passing the surface's own value rather than a second tuning knob
             * is the point: the fog and the wall in front of it must soften
             * the same boundary by the same amount, or the fog bands where the
             * wall does not. */
            par[1] = sun->cascade_blend;
        }

        bgfx_set_uniform(f->u_sun,        sd,  1);
        bgfx_set_uniform(f->u_sun_color,  sc,  1);
        bgfx_set_uniform(f->u_csm_vp,     JCE_M4_PTR(vps[0]), 4);
        bgfx_set_uniform(f->u_csm_splits, spl, 1);
        bgfx_set_uniform(f->u_csm_params, par, 1);

        /* Every cascade stage gets a texture whether or not the lookup runs:
         * an unbound sampler is undefined on several backends, and "undefined"
         * here means the fog is shadowed by garbage. */
        for (uint32_t i = 0; i < 4u; i++) {
            bgfx_texture_handle_t t = { UINT16_MAX };
            if (on && i < sun->cascade_count) t.idx = sun->cascade_tex[i];
            if (!BGFX_HANDLE_IS_VALID(t)) t.idx = depth_tex_handle;
            bgfx_set_texture((uint8_t)(1u + i), f->s_csm[i], t,
                             BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                             | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                             | BGFX_SAMPLER_MIP_POINT);
        }
    }

    bgfx_texture_handle_t depth = { depth_tex_handle };
    bgfx_set_texture(0, f->s_depth, depth,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                     | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);

    jce_fs_quad_bind(&f->quad);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(v, f->prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

static bgfx_program_handle_t vfog_screen_program(const JcePakArchive *pak,
    const char *suffix, const char *fragment)
{
    const bgfx_shader_handle_t vs = jce_shader_load_from_pak(pak, "vs_volfog", suffix, LOG_TAG);
    const bgfx_shader_handle_t fs = jce_shader_load_from_pak(pak, fragment, suffix, LOG_TAG);
    if (BGFX_HANDLE_IS_VALID(vs) && BGFX_HANDLE_IS_VALID(fs))
        return bgfx_create_program(vs, fs, true);
    if (BGFX_HANDLE_IS_VALID(vs)) bgfx_destroy_shader(vs);
    if (BGFX_HANDLE_IS_VALID(fs)) bgfx_destroy_shader(fs);
    return (bgfx_program_handle_t){UINT16_MAX};
}

static JceFogScreenShafts *vfog_screen_shafts_get(JceVolumetricFog *f,
    const JcePakArchive *pak)
{
    if (f->screen_shafts_attempted) return f->screen_shafts;
    f->screen_shafts_attempted = true;
    const char *suffix = jce_shader_backend_suffix();
    if (!suffix) return NULL;
    JceFogScreenShafts *s = JCE_CALLOC(1, sizeof *s);
    if (!s) return NULL;

    jce_fs_target_init(&s->target);
    s->radial = vfog_screen_program(pak, suffix, "fs_volfog_screen_shafts");
    s->composite = vfog_screen_program(pak, suffix, "fs_volfog_screen_shafts_composite");
    s->sampler = bgfx_create_uniform("s_screenShafts", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->source = bgfx_create_uniform("u_screenShaftSource", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->tint = bgfx_create_uniform("u_screenShaftTint", BGFX_UNIFORM_TYPE_VEC4, 1);
    if (!BGFX_HANDLE_IS_VALID(s->radial) || !BGFX_HANDLE_IS_VALID(s->composite) ||
        !BGFX_HANDLE_IS_VALID(s->sampler) || !BGFX_HANDLE_IS_VALID(s->source) ||
        !BGFX_HANDLE_IS_VALID(s->tint)) {
        vfog_screen_shafts_destroy(s);
        return NULL;
    }
    f->screen_shafts = s;
    return s;
}

static bool vfog_screen_shafts_target(JceFsTarget *target, int w, int h)
{
    if (BGFX_HANDLE_IS_VALID(target->fb) && target->w == w && target->h == h) return true;
    const uint64_t flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    const bgfx_frame_buffer_handle_t fb = bgfx_create_frame_buffer((uint16_t)w, (uint16_t)h,
        BGFX_TEXTURE_FORMAT_RGBA8, BGFX_TEXTURE_RT | flags);
    if (!BGFX_HANDLE_IS_VALID(fb)) return false;
    const bgfx_texture_handle_t tex = bgfx_get_texture(fb, 0);
    if (!BGFX_HANDLE_IS_VALID(tex)) {
        bgfx_destroy_frame_buffer(fb);
        return false;
    }
    jce_fs_target_destroy(target);
    *target = (JceFsTarget){fb, tex, w, h, BGFX_TEXTURE_FORMAT_RGBA8, flags};
    return true;
}

void jce_volumetric_fog_render_screen_shafts(JceVolumetricFog *f,
    const JcePakArchive *pak, uint16_t depth_tex_handle,
    const jce_mat4 *view, const jce_mat4 *proj,
    jce_vec3 sun_direction, jce_vec3 sun_radiance, float strength,
    const JceVolumetricFogCloudShadow *cloud, uint16_t view_id)
{
    if (!f || !f->composite_ok || !BGFX_HANDLE_IS_VALID(f->rt.fb) ||
        depth_tex_handle == UINT16_MAX || !view || !proj ||
        !(strength > 0.0f) || !(sun_direction.y > 0.0f) ||
        !isfinite(sun_radiance.x) || !isfinite(sun_radiance.y) || !isfinite(sun_radiance.z) ||
        !(fmaxf(fmaxf(sun_radiance.x, sun_radiance.y), sun_radiance.z) > 0.0f))
        return;

    const jce_mat4 vp = jce_m4_multiply(proj, view);
    const jce_vec4 clip = jce_m4_mul_v4(&vp,
        (jce_vec4){sun_direction.x, sun_direction.y, sun_direction.z, 0.0f});
    if (!(clip.w > 1e-4f) || !isfinite(clip.w) || !isfinite(clip.x) || !isfinite(clip.y))
        return;
    const float x = clip.x / clip.w, y = clip.y / clip.w;
    const float margin = fmaxf(fabsf(x), fabsf(y));
    if (!(margin < 1.36f)) return;
    const float edge = fmaxf(margin - 1.0f, 0.0f) / 0.36f;
    const float fade = 1.0f - edge * edge * (3.0f - 2.0f * edge);

    const jce_vec4 n = jce_m4_mul_v4(proj, (jce_vec4){0, 0, -f->params.near_plane, 1});
    const jce_vec4 z = jce_m4_mul_v4(proj, (jce_vec4){0, 0, -f->params.far_plane, 1});
    if (n.w == 0.0f || z.w == 0.0f) return;
    const float near_ndc = n.z / n.w, far_ndc = z.z / z.w;
    if (!isfinite(near_ndc) || !isfinite(far_ndc) || near_ndc == far_ndc) return;
    const float far_depth = near_ndc > far_ndc ? 0.0f : 1.0f;

    JceFogScreenShafts *s = vfog_screen_shafts_get(f, pak);
    if (!s || !vfog_screen_shafts_target(&s->target, (f->rt.w + 3) / 4, (f->rt.h + 3) / 4))
        return;
    if (s->target.tex.idx == depth_tex_handle ||
        (cloud && cloud->extent_m > 0.0f && s->target.tex.idx == cloud->texture)) return;

    const float source[4] = {x, y, far_depth, (float)f->rt.w / f->rt.h};
    const float gain = fminf(strength, 4.0f) * fade;
    s->radiance[0] = fminf(fmaxf(sun_radiance.x, 0.0f), 8.0f) * gain;
    s->radiance[1] = fminf(fmaxf(sun_radiance.y, 0.0f), 8.0f) * gain;
    s->radiance[2] = fminf(fmaxf(sun_radiance.z, 0.0f), 8.0f) * gain;
    s->radiance[3] = 0.0f;

    float map[4] = {0}, ray[4] = {0};
    bgfx_texture_handle_t cloud_tex = {depth_tex_handle};
    if (cloud && cloud->texture != UINT16_MAX && cloud->extent_m > 0.0f) {
        cloud_tex.idx = cloud->texture;
        map[0] = cloud->extent_m;
        map[1] = cloud->center_xz[0];
        map[2] = cloud->center_xz[1];
        map[3] = cloud->base_y_m;
        ray[0] = cloud->sun_slope_xz[0];
        ray[1] = cloud->sun_slope_xz[1];
    }
    if (!BGFX_HANDLE_IS_VALID(f->s_depth) || !BGFX_HANDLE_IS_VALID(f->s_cloud_shadow) ||
        !BGFX_HANDLE_IS_VALID(f->u_cloud_map) || !BGFX_HANDLE_IS_VALID(f->u_cloud_ray))
        return;

    bgfx_set_view_name(view_id, "Scene/ScreenSunShafts", INT32_MAX);
    bgfx_set_view_frame_buffer(view_id, s->target.fb);
    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)s->target.w, (uint16_t)s->target.h);
    bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_transform(view_id, JCE_M4_PTR(*view), JCE_M4_PTR(*proj));

    bgfx_set_uniform(s->source, source, 1);
    bgfx_set_uniform(f->u_cloud_map, map, 1);
    bgfx_set_uniform(f->u_cloud_ray, ray, 1);
    const uint32_t clamp_uv = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    bgfx_set_texture(0, f->s_depth, (bgfx_texture_handle_t){depth_tex_handle},
                     clamp_uv | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT);
    bgfx_set_texture(1, f->s_cloud_shadow, cloud_tex, clamp_uv);
    jce_fs_quad_bind(&f->quad);
    bgfx_set_state(BGFX_STATE_WRITE_RGB, 0);
    bgfx_submit(view_id, s->radial, 0, BGFX_DISCARD_ALL);
    f->screen_shafts_rendered = true;
}

uint16_t jce_volumetric_fog_get_result_texture(const JceVolumetricFog *f)
{
    if (!f || f->rt.fb.idx == UINT16_MAX) return UINT16_MAX;
    return f->rt.tex.idx;
}

void jce_volumetric_fog_composite(JceVolumetricFog *f,
                                  uint16_t view_id,
                                  uint16_t dst_fb_idx)
{
    if (!f || !f->composite_ok)                    return;
    if (f->prog_composite.idx == UINT16_MAX)       return;
    if (f->rt.fb.idx == UINT16_MAX)                   return;
    JCE_PROFILE_ZONE_N("Renderer::VolumetricFog::composite");

    /* Bind the destination FBO and view rect explicitly. Callers must
     * pass a view-id strictly greater than the fog-render view-id used
     * in jce_volumetric_fog_render(); otherwise composite would sample
     * stale data from the previous frame's fog RT. */
    bgfx_set_view_name(view_id, "Scene/FogComposite", INT32_MAX);
    bgfx_frame_buffer_handle_t dst = { dst_fb_idx };
    bgfx_set_view_frame_buffer(view_id, dst);
    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)f->rt.w, (uint16_t)f->rt.h);
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);

    bgfx_set_texture(0, f->s_fog, f->rt.tex,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                     | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);

    jce_fs_quad_bind(&f->quad);

    /* dst.rgb = dst.rgb * fog.a + fog.rgb
     * src factor = ONE, dst factor = SRC_ALPHA. */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                           BGFX_STATE_BLEND_SRC_ALPHA);
    bgfx_set_state(state, 0);
    bgfx_program_handle_t program = f->prog_composite;
    JceFogScreenShafts *shaft = f->screen_shafts;
    if (f->screen_shafts_rendered && shaft && shaft->target.fb.idx != dst_fb_idx) {
        bgfx_set_texture(1, shaft->sampler, shaft->target.tex,
                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        bgfx_set_uniform(shaft->tint, shaft->radiance, 1);
        program = shaft->composite;
    }
    bgfx_submit(view_id, program, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}
