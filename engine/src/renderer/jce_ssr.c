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

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "ssr"

typedef struct { float pos[2]; float uv[2]; } SsrQuadV;

struct JceSsr {
    int  w, h;
    JceSsrParams params;

    bgfx_vertex_layout_t   layout;
    bgfx_program_handle_t  prog;

    bgfx_uniform_handle_t  u_p0;
    bgfx_uniform_handle_t  u_p1;
    bgfx_uniform_handle_t  u_screen;
    bgfx_uniform_handle_t  s_color, s_depth, s_normal;

    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;

    bgfx_frame_buffer_handle_t fb;
    bgfx_texture_handle_t      tex;
};

static const char *ssr_backend_suffix(void)
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

static bgfx_shader_handle_t ssr_load_shader(const JcePakArchive *pak,
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

static void create_target(JceSsr *s)
{
    if (s->fb.idx != UINT16_MAX) bgfx_destroy_frame_buffer(s->fb);
    s->fb = bgfx_create_frame_buffer((uint16_t)s->w, (uint16_t)s->h,
                                     BGFX_TEXTURE_FORMAT_RGBA8,
                                     BGFX_TEXTURE_RT
                                       | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    s->tex = bgfx_get_texture(s->fb, 0);
}

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
    const char *sfx = ssr_backend_suffix();
    if (!sfx) return NULL;

    JceSsr *s = (JceSsr *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;
    s->w = desc->width  > 0 ? desc->width  : 1280;
    s->h = desc->height > 0 ? desc->height : 720;
    s->params = jce_ssr_default_params();
    s->fb.idx = UINT16_MAX;

    bgfx_vertex_layout_begin(&s->layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&s->layout, BGFX_ATTRIB_POSITION,  2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&s->layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&s->layout);

    static const SsrQuadV verts[4] = {
        { { -1.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  1.0f, -1.0f }, { 1.0f, 1.0f } },
        { {  1.0f,  1.0f }, { 1.0f, 0.0f } },
        { { -1.0f,  1.0f }, { 0.0f, 0.0f } },
    };
    static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    s->vbh = bgfx_create_vertex_buffer(bgfx_copy(verts, sizeof(verts)),
                                       &s->layout, BGFX_BUFFER_NONE);
    s->ibh = bgfx_create_index_buffer(bgfx_copy(idx, sizeof(idx)), BGFX_BUFFER_NONE);

    bgfx_shader_handle_t vsh = ssr_load_shader(desc->pak, "vs_ssr", sfx);
    bgfx_shader_handle_t fsh = ssr_load_shader(desc->pak, "fs_ssr", sfx);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed");
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        bgfx_destroy_vertex_buffer(s->vbh);
        bgfx_destroy_index_buffer(s->ibh);
        JCE_FREE(s);
        return NULL;
    }
    s->prog = bgfx_create_program(vsh, fsh, true);

    s->u_p0          = bgfx_create_uniform("u_ssr_params0", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_p1          = bgfx_create_uniform("u_ssr_params1", BGFX_UNIFORM_TYPE_VEC4, 1);
    s->u_screen      = bgfx_create_uniform("u_screen",      BGFX_UNIFORM_TYPE_VEC4, 1);
    s->s_color       = bgfx_create_uniform("s_color",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_depth       = bgfx_create_uniform("s_depth",       BGFX_UNIFORM_TYPE_SAMPLER, 1);
    s->s_normal      = bgfx_create_uniform("s_normal",      BGFX_UNIFORM_TYPE_SAMPLER, 1);

    create_target(s);
    return s;
}

void jce_ssr_destroy(JceSsr *s)
{
    if (!s) return;
    if (s->prog.idx != UINT16_MAX) bgfx_destroy_program(s->prog);
    if (s->u_p0.idx          != UINT16_MAX) bgfx_destroy_uniform(s->u_p0);
    if (s->u_p1.idx          != UINT16_MAX) bgfx_destroy_uniform(s->u_p1);
    if (s->u_screen.idx      != UINT16_MAX) bgfx_destroy_uniform(s->u_screen);
    if (s->s_color.idx       != UINT16_MAX) bgfx_destroy_uniform(s->s_color);
    if (s->s_depth.idx       != UINT16_MAX) bgfx_destroy_uniform(s->s_depth);
    if (s->s_normal.idx      != UINT16_MAX) bgfx_destroy_uniform(s->s_normal);
    if (s->vbh.idx != UINT16_MAX) bgfx_destroy_vertex_buffer(s->vbh);
    if (s->ibh.idx != UINT16_MAX) bgfx_destroy_index_buffer(s->ibh);
    if (s->fb.idx  != UINT16_MAX) bgfx_destroy_frame_buffer(s->fb);
    JCE_FREE(s);
}

void jce_ssr_resize(JceSsr *s, int w, int h)
{
    if (!s || w <= 0 || h <= 0) return;
    if (w == s->w && h == s->h) return;
    s->w = w; s->h = h;
    create_target(s);
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
    bgfx_set_view_frame_buffer(v, s->fb);
    bgfx_set_view_rect(v, 0, 0, (uint16_t)s->w, (uint16_t)s->h);
    bgfx_set_view_clear(v, BGFX_CLEAR_COLOR, 0x00000000, 1.0f, 0);
    /* bgfx auto-fills u_view, u_proj, u_invViewProj from this. */
    bgfx_set_view_transform(v, JCE_M4_PTR(*view), JCE_M4_PTR(*proj));
    bgfx_touch(v);

    float p0[4] = { s->params.max_distance, s->params.thickness,
                    s->params.near_plane,   s->params.far_plane };
    float p1[4] = { s->params.step_count, 0.0f, 0.0f, s->params.intensity };
    float screen[4] = { (float)s->w, (float)s->h,
                        1.0f / (float)s->w, 1.0f / (float)s->h };

    bgfx_set_uniform(s->u_p0, p0, 1);
    bgfx_set_uniform(s->u_p1, p1, 1);
    bgfx_set_uniform(s->u_screen, screen, 1);

    bgfx_texture_handle_t color  = { color_tex_handle  };
    bgfx_texture_handle_t depth  = { depth_tex_handle  };
    bgfx_texture_handle_t normal = { normal_tex_handle };
    bgfx_set_texture(0, s->s_color,  color,  UINT32_MAX);
    bgfx_set_texture(1, s->s_depth,  depth,  UINT32_MAX);
    bgfx_set_texture(2, s->s_normal, normal, UINT32_MAX);

    bgfx_set_vertex_buffer(0, s->vbh, 0, 4);
    bgfx_set_index_buffer(s->ibh, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A, 0);
    bgfx_submit(v, s->prog, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

uint16_t jce_ssr_get_result_texture(const JceSsr *s)
{
    if (!s || s->fb.idx == UINT16_MAX) return UINT16_MAX;
    return s->tex.idx;
}
