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
#include <jce/renderer/jce_volumetric_fog.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_fullscreen_pass.h"
#include "renderer/jce_shader_load.h"   /* backend suffix + engine-pak fallback */

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "volfog"

typedef struct { float pos[2]; float uv[2]; } VfQuadV;

struct JceVolumetricFog {
    JceVolumetricFogParams params;

    bgfx_program_handle_t  prog;

    bgfx_uniform_handle_t  u_p0;
    bgfx_uniform_handle_t  u_p1;
    bgfx_uniform_handle_t  u_color;
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

void jce_volumetric_fog_destroy(JceVolumetricFog *f)
{
    if (!f) return;
    if (f->prog.idx           != UINT16_MAX) bgfx_destroy_program(f->prog);
    if (f->prog_composite.idx != UINT16_MAX) bgfx_destroy_program(f->prog_composite);
    if (f->u_p0.idx           != UINT16_MAX) bgfx_destroy_uniform(f->u_p0);
    if (f->u_p1.idx           != UINT16_MAX) bgfx_destroy_uniform(f->u_p1);
    if (f->u_color.idx        != UINT16_MAX) bgfx_destroy_uniform(f->u_color);
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
    if (q.step_count     <  1.0f) q.step_count     = 1.0f;
    if (q.step_count     > 256.f) q.step_count     = 256.0f;
    if (q.near_plane     <= 0.0f) q.near_plane     = 0.01f;
    if (q.far_plane      <= q.near_plane) q.far_plane = q.near_plane + 1.0f;
    if (q.color_r        <  0.0f) q.color_r        = 0.0f;
    if (q.color_g        <  0.0f) q.color_g        = 0.0f;
    if (q.color_b        <  0.0f) q.color_b        = 0.0f;
    f->params = q;
}

void jce_volumetric_fog_render(JceVolumetricFog *f,
                               uint16_t depth_tex_handle,
                               const jce_mat4 *view, const jce_mat4 *proj,
                               const JceVolumetricFogSun *sun,
                               uint16_t first_view_id)
{
    if (!f || f->prog.idx == UINT16_MAX || !view || !proj) return;
    if (depth_tex_handle == UINT16_MAX) return;
    JCE_PROFILE_ZONE_N("Renderer::VolumetricFog::render");

    uint16_t v = first_view_id;
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
    bgfx_submit(view_id, f->prog_composite, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}
