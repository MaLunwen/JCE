/*
 * jce_ssr.c -- screen-space reflections.
 *
 * Single full-screen pass: linear ray-march in view space along the
 * reflected vector, sample depth at each step's projected NDC, hit when
 * delta within `thickness`.  Output RGBA: rgb = mirrored color, a = fade.
 */

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_ssr.h>
#include <jce/renderer/jce_views.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_fullscreen_pass.h"
#include "renderer/jce_shader_load.h"   /* backend suffix + engine-pak fallback */

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "ssr"

struct JceSsr {
    JceSsrParams params;

    bgfx_program_handle_t  prog;
    bgfx_program_handle_t  prog_composite;   /* fs_ssr_composite blend pass */

    bgfx_uniform_handle_t  u_p0;
    bgfx_uniform_handle_t  u_p1;
    bgfx_uniform_handle_t  u_screen;
    bgfx_uniform_handle_t  s_color, s_depth, s_normal;

    /* Shared with SSAO, volumetric fog and SSGI: jce_fullscreen_pass.h. */
    JceFsQuad   quad;
    JceFsTarget rt;
};

JceSsrParams jce_ssr_default_params(void)
{
    JceSsrParams p;
    p.max_distance = 50.0f;
    p.thickness    = 0.5f;
    p.step_count   = 32.0f;
    p.intensity    = 1.0f;
    p.near_plane   = 0.1f;
    p.far_plane    = 1000.0f;
    return p;
}

JceSsr *jce_ssr_create(const JceSsrDesc *desc)
{
    if (!desc || !desc->pak) return NULL;
    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) return NULL;

    JceSsr *s = (JceSsr *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    s->params = jce_ssr_default_params();
    jce_fs_target_init(&s->rt);
    jce_fs_quad_init(&s->quad);
    const int want_w = desc->width  > 0 ? desc->width  : 1280;
    const int want_h = desc->height > 0 ? desc->height : 720;

    bgfx_shader_handle_t vsh = jce_shader_load_from_pak(desc->pak, "vs_ssr", sfx, LOG_TAG);
    bgfx_shader_handle_t fsh = jce_shader_load_from_pak(desc->pak, "fs_ssr", sfx, LOG_TAG);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed");
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        jce_fs_quad_destroy(&s->quad);
        JCE_FREE(s);
        return NULL;
    }
    s->prog = bgfx_create_program(vsh, fsh, true);

    /* Composite program: vs_ssr + fs_ssr_composite (loaded fresh — the create
     * above destroyed vsh/fsh).  Optional; absent => composite is a no-op. */
    s->prog_composite.idx = UINT16_MAX;
    {
        bgfx_shader_handle_t cvsh = jce_shader_load_from_pak(desc->pak, "vs_ssr",           sfx, LOG_TAG);
        bgfx_shader_handle_t cfsh = jce_shader_load_from_pak(desc->pak, "fs_ssr_composite", sfx, LOG_TAG);
        if (cvsh.idx != UINT16_MAX && cfsh.idx != UINT16_MAX) {
            s->prog_composite = bgfx_create_program(cvsh, cfsh, true);
        } else {
            if (cvsh.idx != UINT16_MAX) bgfx_destroy_shader(cvsh);
            if (cfsh.idx != UINT16_MAX) bgfx_destroy_shader(cfsh);
        }
    }

    s->u_p0          = bgfx_create_uniform("u_ssr_params0", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_p1          = bgfx_create_uniform("u_ssr_params1", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_screen      = bgfx_create_uniform("u_screen",      BGFX_UNIFORM_TYPE_VEC4, 1);
    s->s_color       = bgfx_create_uniform("s_color",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_depth       = bgfx_create_uniform("s_depth",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_normal      = bgfx_create_uniform("s_normal",      BGFX_UNIFORM_TYPE_SAMPLER, 1);

    jce_fs_target_create(&s->rt, want_w, want_h, BGFX_TEXTURE_FORMAT_RGBA8,
                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    return s;
}

void jce_ssr_destroy(JceSsr *s)
{
    if (!s) return;
    if (s->prog.idx != UINT16_MAX) bgfx_destroy_program(s->prog);
    if (s->prog_composite.idx != UINT16_MAX) bgfx_destroy_program(s->prog_composite);
    if (s->u_p0.idx          != UINT16_MAX) bgfx_destroy_uniform(s->u_p0);
    if (s->u_p1.idx          != UINT16_MAX) bgfx_destroy_uniform(s->u_p1);
    if (s->u_screen.idx      != UINT16_MAX) bgfx_destroy_uniform(s->u_screen);
    if (s->s_color.idx       != UINT16_MAX) bgfx_destroy_uniform(s->s_color);
    if (s->s_depth.idx       != UINT16_MAX) bgfx_destroy_uniform(s->s_depth);
    if (s->s_normal.idx      != UINT16_MAX) bgfx_destroy_uniform(s->s_normal);
    jce_fs_quad_destroy(&s->quad);
    jce_fs_target_destroy(&s->rt);
    JCE_FREE(s);
}

void jce_ssr_resize(JceSsr *s, int w, int h)
{
    if (s) jce_fs_target_resize(&s->rt, w, h);
}

void jce_ssr_set_params(JceSsr *s, const JceSsrParams *p)
{
    if (!s || !p) return;
    JceSsrParams q = *p;
    if (q.max_distance <= 0.0f) q.max_distance = 1.0f;
    if (q.thickness    <= 0.0f) q.thickness    = 0.01f;
    if (q.step_count   <  1.0f) q.step_count   = 1.0f;
    if (q.step_count   > 256.f) q.step_count   = 256.0f;
    if (q.intensity    <  0.0f) q.intensity    = 0.0f;
    if (q.near_plane   <= 0.0f) q.near_plane   = 0.01f;
    if (q.far_plane    <= q.near_plane) q.far_plane = q.near_plane + 1.0f;
    s->params = q;
}

void jce_ssr_render(JceSsr *s,
                    uint16_t color_tex_handle,
                    uint16_t depth_tex_handle,
                    uint16_t normal_tex_handle,
                    const jce_mat4 *view, const jce_mat4 *proj,
                    uint16_t first_view_id)
{
    if (!s || s->prog.idx == UINT16_MAX || !view || !proj) return;
    if (color_tex_handle  == UINT16_MAX ||
        depth_tex_handle  == UINT16_MAX ||
        normal_tex_handle == UINT16_MAX) return;
    JCE_PROFILE_ZONE_N("Renderer::SSR::render");

    uint16_t v = first_view_id;
    bgfx_set_view_frame_buffer(v, s->rt.fb);
    bgfx_set_view_rect(v, 0, 0, (uint16_t)s->rt.w, (uint16_t)s->rt.h);
    bgfx_set_view_clear(v, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
    /* bgfx auto-fills u_view, u_proj, u_invViewProj from this. */
    bgfx_set_view_transform(v, JCE_M4_PTR(*view), JCE_M4_PTR(*proj));
    bgfx_touch(v);

    float p0[4] = { s->params.max_distance, s->params.thickness,
                    s->params.near_plane,   s->params.far_plane };
    float p1[4] = { s->params.step_count, 0.0f, 0.0f, s->params.intensity };
    float screen[4] = { (float)s->rt.w, (float)s->rt.h,
                        1.0f / (float)s->rt.w, 1.0f / (float)s->rt.h };

    bgfx_set_uniform(s->u_p0, p0, 1);
    bgfx_set_uniform(s->u_p1, p1, 1);
    bgfx_set_uniform(s->u_screen, screen, 1);

    bgfx_texture_handle_t color  = { color_tex_handle  };
    bgfx_texture_handle_t depth  = { depth_tex_handle  };
    bgfx_texture_handle_t normal = { normal_tex_handle };
    bgfx_set_texture(0, s->s_color,  color,  UINT32_MAX);
    bgfx_set_texture(1, s->s_depth,  depth,  UINT32_MAX);
    bgfx_set_texture(2, s->s_normal, normal, UINT32_MAX);

    jce_fs_quad_bind(&s->quad);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(v, s->prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

uint16_t jce_ssr_get_result_texture(const JceSsr *s)
{
    if (!s || s->rt.fb.idx == UINT16_MAX) return UINT16_MAX;
    return s->rt.tex.idx;
}

void jce_ssr_composite(JceSsr *s, uint16_t view_id, uint16_t dst_fb_idx)
{
    if (!s || s->prog_composite.idx == UINT16_MAX) return;
    if (s->rt.tex.idx == UINT16_MAX)               return;
    JCE_PROFILE_ZONE_N("Renderer::SSR::composite");

    /* Blend the reflection RT over the destination color buffer.  Callers must
     * pass a view-id strictly greater than the SSR ray-march view so the RT is
     * filled first. */
    bgfx_frame_buffer_handle_t dst = { dst_fb_idx };
    bgfx_set_view_frame_buffer(view_id, dst);
    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)s->rt.w, (uint16_t)s->rt.h);
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);

    bgfx_set_texture(0, s->s_color, s->rt.tex,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                     | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);
    jce_fs_quad_bind(&s->quad);

    /* Premultiplied "over": dst = refl.rgb + dst*(1-refl.a). */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA);
    bgfx_set_state(state, 0);
    bgfx_submit(view_id, s->prog_composite, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}
