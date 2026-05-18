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

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "volfog"

typedef struct { float pos[2]; float uv[2]; } VfQuadV;

struct JceVolumetricFog {
    int  w, h;
    JceVolumetricFogParams params;

    bgfx_vertex_layout_t   layout;
    bgfx_program_handle_t  prog;

    bgfx_uniform_handle_t  u_p0;
    bgfx_uniform_handle_t  u_p1;
    bgfx_uniform_handle_t  u_color;
    bgfx_uniform_handle_t  s_depth;

    /* Composite pass — draws the fog RT into a destination frame
     * buffer with blend ONE/SRC_ALPHA. */
    bgfx_program_handle_t  prog_composite;
    bgfx_uniform_handle_t  s_fog;
    bool                   composite_ok;

    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;

    bgfx_frame_buffer_handle_t fb;
    bgfx_texture_handle_t      tex;
};

static const char *vf_backend_suffix(void)
{
    switch (bgfx_get_renderer_type()) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12: return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:     return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:     return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:   return "essl";
    case BGFX_RENDERER_TYPE_METAL:      return "mtl";
    default:                            return NULL;
    }
}

static bgfx_shader_handle_t vf_load_shader(const JcePakArchive *pak,
                                           const char *name, const char *sfx)
{
    bgfx_shader_handle_t invalid = { UINT16_MAX };
    char path[256];
    snprintf(path, sizeof(path), "shaders/%s_%s.bin", name, sfx);
    const JcePakAsset *a = jce_pak_find(pak, path);
    if (!a) { LOG_ERROR(LOG_TAG, "shader not in pak: %s", path); return invalid; }
    void *buf = JCE_MALLOC((size_t)a->original_size);
    if (!buf) return invalid;
    size_t n = jce_pak_decompress(a, buf, (size_t)a->original_size);
    if (n == 0) { JCE_FREE(buf); return invalid; }
    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)a->original_size);
    JCE_FREE(buf);
    return bgfx_create_shader(mem);
}

static void create_target(JceVolumetricFog *f)
{
    if (f->fb.idx != UINT16_MAX) bgfx_destroy_frame_buffer(f->fb);
    f->fb = bgfx_create_frame_buffer((uint16_t)f->w, (uint16_t)f->h,
                                     BGFX_TEXTURE_FORMAT_RGBA8,
                                     BGFX_TEXTURE_RT
                                       | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    f->tex = bgfx_get_texture(f->fb, 0);
}

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
    const char *sfx = vf_backend_suffix();
    if (!sfx) return NULL;

    JceVolumetricFog *f = (JceVolumetricFog *)JCE_CALLOC(1, sizeof(*f));
    if (!f) return NULL;
    f->w = desc->width  > 0 ? desc->width  : 1280;
    f->h = desc->height > 0 ? desc->height : 720;
    f->params = jce_volumetric_fog_default_params();
    f->fb.idx = UINT16_MAX;

    bgfx_vertex_layout_begin(&f->layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&f->layout, BGFX_ATTRIB_POSITION,  2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&f->layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&f->layout);

    static const VfQuadV verts[4] = {
        { { -1.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  1.0f, -1.0f }, { 1.0f, 1.0f } },
        { {  1.0f,  1.0f }, { 1.0f, 0.0f } },
        { { -1.0f,  1.0f }, { 0.0f, 0.0f } },
    };
    static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    f->vbh = bgfx_create_vertex_buffer(bgfx_copy(verts, sizeof(verts)),
                                       &f->layout, BGFX_BUFFER_NONE);
    f->ibh = bgfx_create_index_buffer(bgfx_copy(idx, sizeof(idx)), BGFX_BUFFER_NONE);

    bgfx_shader_handle_t vsh = vf_load_shader(desc->pak, "vs_volfog", sfx);
    bgfx_shader_handle_t fsh = vf_load_shader(desc->pak, "fs_volfog", sfx);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed");
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        bgfx_destroy_vertex_buffer(f->vbh);
        bgfx_destroy_index_buffer(f->ibh);
        JCE_FREE(f);
        return NULL;
    }
    f->prog = bgfx_create_program(vsh, fsh, true);

    f->u_p0    = bgfx_create_uniform("u_volfog_p0",    BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_p1    = bgfx_create_uniform("u_volfog_p1",    BGFX_UNIFORM_TYPE_VEC4, 1);
    f->u_color = bgfx_create_uniform("u_volfog_color", BGFX_UNIFORM_TYPE_VEC4, 1);
    f->s_depth = bgfx_create_uniform("s_depth",        BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Composite pass program (vs_volfog reused). Optional — if the
     * shader binary is missing on this backend the renderer still
     * renders fog into its private RT and callers can fall back to
     * jce_scene_renderer_get_fog_result_texture(). */
    f->prog_composite.idx = UINT16_MAX;
    f->s_fog.idx          = UINT16_MAX;
    f->composite_ok       = false;
    {
        bgfx_shader_handle_t cvsh = vf_load_shader(desc->pak, "vs_volfog", sfx);
        bgfx_shader_handle_t cfsh = vf_load_shader(desc->pak, "fs_volfog_composite", sfx);
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

    create_target(f);
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
    if (f->s_depth.idx        != UINT16_MAX) bgfx_destroy_uniform(f->s_depth);
    if (f->s_fog.idx          != UINT16_MAX) bgfx_destroy_uniform(f->s_fog);
    if (f->vbh.idx      != UINT16_MAX) bgfx_destroy_vertex_buffer(f->vbh);
    if (f->ibh.idx      != UINT16_MAX) bgfx_destroy_index_buffer(f->ibh);
    if (f->fb.idx       != UINT16_MAX) bgfx_destroy_frame_buffer(f->fb);
    JCE_FREE(f);
}

void jce_volumetric_fog_resize(JceVolumetricFog *f, int w, int h)
{
    if (!f || w <= 0 || h <= 0) return;
    if (w == f->w && h == f->h) return;
    f->w = w; f->h = h;
    create_target(f);
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
                               uint16_t first_view_id)
{
    if (!f || f->prog.idx == UINT16_MAX || !view || !proj) return;
    if (depth_tex_handle == UINT16_MAX) return;
    JCE_PROFILE_ZONE_N("Renderer::VolumetricFog::render");

    uint16_t v = first_view_id;
    bgfx_set_view_frame_buffer(v, f->fb);
    bgfx_set_view_rect(v, 0, 0, (uint16_t)f->w, (uint16_t)f->h);
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

    bgfx_texture_handle_t depth = { depth_tex_handle };
    bgfx_set_texture(0, f->s_depth, depth,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                     | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);

    bgfx_set_vertex_buffer(0, f->vbh, 0, 4);
    bgfx_set_index_buffer(f->ibh, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(v, f->prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

uint16_t jce_volumetric_fog_get_result_texture(const JceVolumetricFog *f)
{
    if (!f || f->fb.idx == UINT16_MAX) return UINT16_MAX;
    return f->tex.idx;
}

void jce_volumetric_fog_composite(JceVolumetricFog *f,
                                  uint16_t view_id,
                                  uint16_t dst_fb_idx)
{
    if (!f || !f->composite_ok)                    return;
    if (f->prog_composite.idx == UINT16_MAX)       return;
    if (f->fb.idx == UINT16_MAX)                   return;
    JCE_PROFILE_ZONE_N("Renderer::VolumetricFog::composite");

    /* Bind the destination FBO and view rect explicitly. Callers must
     * pass a view-id strictly greater than the fog-render view-id used
     * in jce_volumetric_fog_render(); otherwise composite would sample
     * stale data from the previous frame's fog RT. */
    bgfx_frame_buffer_handle_t dst = { dst_fb_idx };
    bgfx_set_view_frame_buffer(view_id, dst);
    bgfx_set_view_rect(view_id, 0, 0, (uint16_t)f->w, (uint16_t)f->h);
    bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
    bgfx_touch(view_id);

    bgfx_set_texture(0, f->s_fog, f->tex,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
                     | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                     | BGFX_SAMPLER_MIP_POINT);

    bgfx_set_vertex_buffer(0, f->vbh, 0, 4);
    bgfx_set_index_buffer(f->ibh, 0, 6);

    /* dst.rgb = dst.rgb * fog.a + fog.rgb
     * src factor = ONE, dst factor = SRC_ALPHA. */
    uint64_t state = BGFX_STATE_WRITE_RGB
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE,
                                           BGFX_STATE_BLEND_SRC_ALPHA);
    bgfx_set_state(state, 0);
    bgfx_submit(view_id, f->prog_composite, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}
