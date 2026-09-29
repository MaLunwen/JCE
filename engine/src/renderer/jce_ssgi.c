/*
 * jce_ssgi.c -- screen-space global illumination, one diffuse bounce.
 *
 * Deliberately the same shape as jce_ssr.c: fullscreen quad, one march view,
 * one composite view, one offscreen RT.  The two differ in exactly three places,
 * and each difference is a decision rather than an accident:
 *
 *   * the normal input is REAL, not reconstructed.  SSR is handed the depth
 *     texture twice and derives normals in the shader; a diffuse gather needs
 *     the receiver's normal to be right or the hemisphere is wrong, so this
 *     takes the depth pre-pass's normal target.
 *   * the composite is ADDITIVE, not premultiplied "over".  A reflection
 *     replaces what a mirror shows; a bounce is extra light on an already-lit
 *     surface.
 *   * the RT is RGBA16F, not RGBA8.  The bounce is added to an HDR colour
 *     buffer before tonemapping, and clamping it to [0,1] first would cap the
 *     effect at exactly the point where a bright bounce matters.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_ssgi.h>
#include <jce/renderer/jce_views.h>
#include <jce/resource/jce_pak_loader.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_fullscreen_pass.h"
#include "renderer/jce_shader_load.h"   /* backend suffix + engine-pak fallback */

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <string.h>

#define LOG_TAG "ssgi"

struct JceSsgi {
    JceSsgiParams params;

    bgfx_program_handle_t  prog;
    bgfx_program_handle_t  prog_composite;

    bgfx_uniform_handle_t  u_p0;
    bgfx_uniform_handle_t  u_p1;
    bgfx_uniform_handle_t  u_screen;
    bgfx_uniform_handle_t  s_color, s_depth, s_normal, s_albedo;

    /* The quad and the offscreen target are the parts four screen-space
     * effects had a copy of each; see jce_fullscreen_pass.h. */
    JceFsQuad   quad;
    JceFsTarget rt;

    /* The G-buffer the MARCH was handed, remembered for the COMPOSITE.
     *
     * The composite now denoises (fs_ssgi_composite.sc), and an edge-aware
     * filter needs the same depth and normal the march used or it is just a
     * blur.  Kept here rather than added to jce_ssgi_composite's signature so
     * the public API and its two call sites -- the editor viewport and
     * jce_default_main -- do not have to agree on a new argument: the two
     * drifting apart is the exact editor-vs-shipped-exe divergence this tree
     * keeps finding.  Reset to invalid on every failed/absent march so a stale
     * pair from a previous frame can never be sampled. */
    bgfx_texture_handle_t  gbuf_depth;
    bgfx_texture_handle_t  gbuf_normal;
};

JceSsgiParams jce_ssgi_default_params(void)
{
    JceSsgiParams p;
    /* 3 m of bounce: far enough to carry a wall's colour onto the floor beside
     * it, short enough that the fixed 8-step march still resolves thin
     * occluders.  Nothing here is tuned to a particular scene. */
    /* ray_count / step_count below are the shader's COMPILED-IN bounds; see
     * the header for why they are small rather than a taste. */
    p.radius     = 3.0f;
    p.thickness  = 0.5f;
    p.ray_count  = 4.0f;
    p.step_count = 8.0f;
    /* Below 1.0 on purpose -- the receiver-colour proxy for albedo (see the
     * shader header) over-counts on bright surfaces, and the honest place to
     * pay for that is the default, not a comment. */
    p.intensity  = 0.6f;
    p.near_plane = 0.1f;
    p.far_plane  = 1000.0f;
    return p;
}

JceSsgi *jce_ssgi_create(const JceSsgiDesc *desc)
{
    if (!desc || !desc->pak) return NULL;
    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) return NULL;

    JceSsgi *s = (JceSsgi *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    s->params = jce_ssgi_default_params();
    s->gbuf_depth.idx  = UINT16_MAX;
    s->gbuf_normal.idx = UINT16_MAX;
    jce_fs_target_init(&s->rt);
    jce_fs_quad_init(&s->quad);
    const int want_w = desc->width  > 0 ? desc->width  : 1280;
    const int want_h = desc->height > 0 ? desc->height : 720;

    bgfx_shader_handle_t vsh = jce_shader_load_from_pak(desc->pak, "vs_ssgi", sfx, LOG_TAG);
    bgfx_shader_handle_t fsh = jce_shader_load_from_pak(desc->pak, "fs_ssgi", sfx, LOG_TAG);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed");
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        jce_fs_quad_destroy(&s->quad);
        JCE_FREE(s);
        return NULL;
    }
    s->prog = bgfx_create_program(vsh, fsh, true);

    /* Composite program.  Optional: absent => composite is a no-op, which is
     * the same tolerance jce_ssr.c has and for the same reason -- a partial
     * shader pak must degrade to "no effect", never to a black screen. */
    s->prog_composite.idx = UINT16_MAX;
    {
        bgfx_shader_handle_t cvsh = jce_shader_load_from_pak(desc->pak, "vs_ssgi",            sfx, LOG_TAG);
        bgfx_shader_handle_t cfsh = jce_shader_load_from_pak(desc->pak, "fs_ssgi_composite", sfx, LOG_TAG);
        if (cvsh.idx != UINT16_MAX && cfsh.idx != UINT16_MAX) {
            s->prog_composite = bgfx_create_program(cvsh, cfsh, true);
        } else {
            if (cvsh.idx != UINT16_MAX) bgfx_destroy_shader(cvsh);
            if (cfsh.idx != UINT16_MAX) bgfx_destroy_shader(cfsh);
        }
    }

    s->u_p0     = bgfx_create_uniform("u_ssgi_params0", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_p1     = bgfx_create_uniform("u_ssgi_params1", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_screen = bgfx_create_uniform("u_screen",       BGFX_UNIFORM_TYPE_VEC4, 1);
    s->s_color  = bgfx_create_uniform("s_color",        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_depth  = bgfx_create_uniform("s_depth",        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_normal = bgfx_create_uniform("s_normal",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_albedo = bgfx_create_uniform("s_albedo", BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* RGBA16F: see the file header.  The bounce is added to an HDR buffer. */
    jce_fs_target_create(&s->rt, want_w, want_h, BGFX_TEXTURE_FORMAT_RGBA16F,
                         BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    return s;
}

void jce_ssgi_destroy(JceSsgi *s)
{
    if (!s) return;
    if (s->prog.idx           != UINT16_MAX) bgfx_destroy_program(s->prog);
    if (s->prog_composite.idx != UINT16_MAX) bgfx_destroy_program(s->prog_composite);
    if (s->u_p0.idx     != UINT16_MAX) bgfx_destroy_uniform(s->u_p0);
    if (s->u_p1.idx     != UINT16_MAX) bgfx_destroy_uniform(s->u_p1);
    if (s->u_screen.idx != UINT16_MAX) bgfx_destroy_uniform(s->u_screen);
    if (s->s_color.idx  != UINT16_MAX) bgfx_destroy_uniform(s->s_color);
    if (s->s_depth.idx  != UINT16_MAX) bgfx_destroy_uniform(s->s_depth);
    if (s->s_normal.idx != UINT16_MAX) bgfx_destroy_uniform(s->s_normal);
    if (s->s_albedo.idx != UINT16_MAX) bgfx_destroy_uniform(s->s_albedo);
    jce_fs_quad_destroy(&s->quad);
    jce_fs_target_destroy(&s->rt);
    JCE_FREE(s);
}

void jce_ssgi_resize(JceSsgi *s, int w, int h)
{
    if (s) jce_fs_target_resize(&s->rt, w, h);
}

void jce_ssgi_set_params(JceSsgi *s, const JceSsgiParams *p)
{
    if (!s || !p) return;
    JceSsgiParams q = *p;
    if (q.radius     <= 0.0f) q.radius     = 0.01f;
    if (q.thickness  <= 0.0f) q.thickness  = 0.01f;
    /* Clamped to the shader's FIXED loop bounds, not to taste: 4 rays and 8
     * steps are compiled in, and a caller asking for more would silently get
     * the bound instead.  Clamping here makes the ceiling visible to whoever
     * reads the value back. */
    if (q.ray_count  < 1.0f) q.ray_count  = 1.0f;
    if (q.ray_count  > 4.0f) q.ray_count  = 4.0f;
    if (q.step_count < 1.0f) q.step_count = 1.0f;
    if (q.step_count > 8.0f) q.step_count = 8.0f;
    if (q.intensity  <  0.0f) q.intensity  = 0.0f;
    if (q.near_plane <= 0.0f) q.near_plane = 0.01f;
    if (q.far_plane  <= q.near_plane) q.far_plane = q.near_plane + 1.0f;
    s->params = q;
}

void jce_ssgi_render(JceSsgi *s,
                     JceTextureHandle color,
                     JceTextureHandle depth,
                     JceTextureHandle normal,
                     const jce_mat4 *view,
                     const jce_mat4 *proj,
                     uint16_t first_view_id)
{
    /* No albedo target: the invalid handle makes the callee bind the
     * colour target in its place, which is what this function always
     * did -- byte for byte. */
    JceTextureHandle none; none.idx = UINT16_MAX;
    jce_ssgi_render_albedo(s, color, depth, normal, none, view, proj,
                           first_view_id);
}

void jce_ssgi_render_albedo(JceSsgi *s,
                            JceTextureHandle color_in,
                            JceTextureHandle depth_in,
                            JceTextureHandle normal_in,
                            JceTextureHandle albedo_in,
                            const jce_mat4 *view,
                            const jce_mat4 *proj,
                            uint16_t first_view_id)
{
    if (!s) return;
    /* Clear the remembered G-buffer BEFORE any early return: a composite that
     * ran after a march that did not must not filter against last frame's
     * depth.  It falls back to the unfiltered fetch instead. */
    s->gbuf_depth.idx  = UINT16_MAX;
    s->gbuf_normal.idx = UINT16_MAX;
    if (s->prog.idx == UINT16_MAX || !view || !proj) return;
    if (color_in.idx  == UINT16_MAX ||
        depth_in.idx  == UINT16_MAX ||
        normal_in.idx == UINT16_MAX) return;
    JCE_PROFILE_ZONE_N("Renderer::SSGI::render");

    uint16_t v = first_view_id;
    bgfx_set_view_frame_buffer(v, s->rt.fb);
    bgfx_set_view_rect(v, 0, 0, (uint16_t)s->rt.w, (uint16_t)s->rt.h);
    bgfx_set_view_clear(v, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
    /* bgfx fills u_view / u_proj / u_invProj / u_invViewProj from this. */
    bgfx_set_view_transform(v, JCE_M4_PTR(*view), JCE_M4_PTR(*proj));
    bgfx_touch(v);

    float p0[4] = { s->params.radius,     s->params.thickness,
                    s->params.near_plane, s->params.far_plane };
    float p1[4] = { s->params.ray_count,  s->params.step_count,
                    s->params.intensity,  0.0f };
    float screen[4] = { (float)s->rt.w, (float)s->rt.h,
                        1.0f / (float)s->rt.w, 1.0f / (float)s->rt.h };

    bgfx_set_uniform(s->u_p0, p0, 1);
    bgfx_set_uniform(s->u_p1, p1, 1);
    bgfx_set_uniform(s->u_screen, screen, 1);

    bgfx_texture_handle_t color  = { color_in.idx  };
    bgfx_texture_handle_t depth  = { depth_in.idx  };
    bgfx_texture_handle_t normal = { normal_in.idx };
    bgfx_set_texture(0, s->s_color,  color,  UINT32_MAX);
    bgfx_set_texture(1, s->s_depth,  depth,  UINT32_MAX);
    bgfx_set_texture(2, s->s_normal, normal, UINT32_MAX);
    /* ALBEDO, or the colour target when there is none -- the shader then
     * reads exactly what it used to read, with no branch and no second
     * program. */
    bgfx_texture_handle_t albedo = { albedo_in.idx != UINT16_MAX
                                     ? albedo_in.idx : color_in.idx };
    bgfx_set_texture(3, s->s_albedo, albedo, UINT32_MAX);
    s->gbuf_depth  = depth;
    s->gbuf_normal = normal;

    jce_fs_quad_bind(&s->quad);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(v, s->prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

JceTextureHandle jce_ssgi_get_result_texture(const JceSsgi *s)
{
    JceTextureHandle h;
    h.idx = (!s || s->rt.fb.idx == UINT16_MAX) ? UINT16_MAX : s->rt.tex.idx;
    return h;
}

void jce_ssgi_composite(JceSsgi *s, uint16_t view_id, JceFrameBufferHandle out)
{
    if (!s || s->prog_composite.idx == UINT16_MAX) return;
    if (s->rt.tex.idx == UINT16_MAX)               return;
    JCE_PROFILE_ZONE_N("Renderer::SSGI::composite");

    bgfx_frame_buffer_handle_t dst = { out.idx };
    bgfx_set_view_frame_buffer(view_id, dst);
    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)s->rt.w, (uint16_t)s->rt.h);
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);

    bgfx_set_texture(0, s->s_color, s->rt.tex,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                     | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);

    /* The denoiser's guides.  When the march did not run this frame the pair
     * is invalid; bind the bounce RT in their place so every sampler has a
     * texture (an unbound sampler reads undefined on some backends) and the
     * shader's own sky test then sees depth >= 0.9999 nowhere -- it degrades
     * to a wide unguided gather rather than to garbage.  This is the
     * shader-pak-is-partial tolerance the file header already claims, applied
     * to the same pass one level down. */
    bgfx_texture_handle_t gd = (s->gbuf_depth.idx  != UINT16_MAX)
                             ? s->gbuf_depth  : s->rt.tex;
    bgfx_texture_handle_t gn = (s->gbuf_normal.idx != UINT16_MAX)
                             ? s->gbuf_normal : s->rt.tex;
    bgfx_set_texture(1, s->s_depth,  gd, UINT32_MAX);
    bgfx_set_texture(2, s->s_normal, gn, UINT32_MAX);

    /* The filter linearises depth, so it needs the same near/far and the same
     * texel size the march was given.  bgfx uniforms are per-submit state, not
     * per-view, so they have to be set again here. */
    float p0[4] = { s->params.radius,     s->params.thickness,
                    s->params.near_plane, s->params.far_plane };
    float screen[4] = { (float)s->rt.w, (float)s->rt.h,
                        1.0f / (float)s->rt.w, 1.0f / (float)s->rt.h };
    bgfx_set_uniform(s->u_p0, p0, 1);
    bgfx_set_uniform(s->u_screen, screen, 1);

    jce_fs_quad_bind(&s->quad);

    /* ADDITIVE: dst += bounce.  See the header for why this is not the "over"
     * blend SSR uses. */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                           BGFX_STATE_BLEND_ONE);
    bgfx_set_state(state, 0);
    bgfx_submit(view_id, s->prog_composite, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}
