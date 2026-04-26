/*
 * jce_primitives.c  2D primitive drawing implementation.
 */

#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_views.h>

#include "jce_renderer_internal.h"

typedef struct { float x, y, z; uint32_t abgr; }             PosColorVertex;
typedef struct { float x, y, z; uint32_t abgr; float u, v; } PosColorTexVertex;

uint32_t jce_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16)
         | ((uint32_t)g <<  8) |  (uint32_t)r;
}

void jce_draw_filled_rect(const JceRenderer *r,
                          float x, float y, float w, float h,
                          uint32_t color)
{
    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout(r);
    bgfx_program_handle_t prog = jce_renderer_get_program(r);
    if (!layout) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;

    if (!bgfx_alloc_transient_buffers(&tvb, layout, 4, &tib, 6, false))
        return;

    PosColorVertex *v = (PosColorVertex *)tvb.data;
    v[0] = (PosColorVertex){ x,     y,     0, color };
    v[1] = (PosColorVertex){ x + w, y,     0, color };
    v[2] = (PosColorVertex){ x + w, y + h, 0, color };
    v[3] = (PosColorVertex){ x,     y + h, 0, color };

    uint16_t *idx = (uint16_t *)tib.data;
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                 | BGFX_STATE_BLEND_ALPHA, 0);
    bgfx_submit(JCE_VIEW_UI, prog, 0, BGFX_DISCARD_ALL);
}

void jce_draw_rect_outline(const JceRenderer *r,
                           float x, float y, float w, float h,
                           uint32_t color)
{
    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout(r);
    bgfx_program_handle_t prog = jce_renderer_get_program(r);
    if (!layout) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;

    if (!bgfx_alloc_transient_buffers(&tvb, layout, 4, &tib, 8, false))
        return;

    PosColorVertex *v = (PosColorVertex *)tvb.data;
    v[0] = (PosColorVertex){ x,     y,     0, color };
    v[1] = (PosColorVertex){ x + w, y,     0, color };
    v[2] = (PosColorVertex){ x + w, y + h, 0, color };
    v[3] = (PosColorVertex){ x,     y + h, 0, color };

    uint16_t *idx = (uint16_t *)tib.data;
    idx[0] = 0; idx[1] = 1;
    idx[2] = 1; idx[3] = 2;
    idx[4] = 2; idx[5] = 3;
    idx[6] = 3; idx[7] = 0;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 8);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                 | BGFX_STATE_BLEND_ALPHA
                 | BGFX_STATE_PT_LINES, 0);
    bgfx_submit(JCE_VIEW_UI, prog, 0, BGFX_DISCARD_ALL);
}

void jce_draw_filled_rects(const JceRenderer *r,
                           const float *rects, int count,
                           uint32_t color)
{
    for (int i = 0; i < count; i++)
        jce_draw_filled_rect(r,
            rects[i*4+0], rects[i*4+1],
            rects[i*4+2], rects[i*4+3], color);
}

void jce_draw_rect_outlines(const JceRenderer *r,
                            const float *rects, int count,
                            uint32_t color)
{
    for (int i = 0; i < count; i++)
        jce_draw_rect_outline(r,
            rects[i*4+0], rects[i*4+1],
            rects[i*4+2], rects[i*4+3], color);
}

void jce_draw_polyline(const JceRenderer *r,
                       const float *points_xy, int point_count,
                       uint32_t color)
{
    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout(r);
    bgfx_program_handle_t prog = jce_renderer_get_program(r);
    if (!layout || !points_xy || point_count < 2)
        return;

    const uint32_t vertex_count = (uint32_t)(point_count - 1) * 2u;
    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t tib;

    if (!bgfx_alloc_transient_buffers(&tvb, layout, vertex_count,
                                      &tib, vertex_count, false))
        return;

    PosColorVertex *v = (PosColorVertex *)tvb.data;
    for (int i = 0; i < point_count - 1; ++i) {
        const int src = i * 2;
        const int dst = i * 2;
        v[dst + 0] = (PosColorVertex){
            points_xy[src + 0], points_xy[src + 1], 0.0f, color
        };
        v[dst + 1] = (PosColorVertex){
            points_xy[src + 2], points_xy[src + 3], 0.0f, color
        };
    }

    {
        uint16_t *idx = (uint16_t *)tib.data;
        for (uint32_t i = 0; i < vertex_count; ++i)
            idx[i] = (uint16_t)i;
    }

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, vertex_count);
    bgfx_set_transient_index_buffer(&tib, 0, vertex_count);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                 | BGFX_STATE_BLEND_ALPHA
                 | BGFX_STATE_PT_LINES, 0);
    bgfx_submit(JCE_VIEW_UI, prog, 0, BGFX_DISCARD_ALL);
}

void jce_draw_textured_rect(const JceRenderer *r,
                            float x, float y, float w, float h,
                            JceTexture tex, uint32_t tint,
                            const float *uv)
{
    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout_textured(r);
    bgfx_program_handle_t prog = jce_renderer_get_program_textured(r);
    JceUniformHandle uh = jce_renderer_get_tex_uniform(r);
    bgfx_uniform_handle_t sampler = { uh.idx };
    if (!layout || prog.idx == UINT16_MAX) return;

    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (uv) { u0 = uv[0]; v0 = uv[1]; u1 = uv[2]; v1 = uv[3]; }

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;

    if (!bgfx_alloc_transient_buffers(&tvb, layout, 4, &tib, 6, false))
        return;

    PosColorTexVertex *v = (PosColorTexVertex *)tvb.data;
    v[0] = (PosColorTexVertex){ x,     y,     0, tint, u0, v0 };
    v[1] = (PosColorTexVertex){ x + w, y,     0, tint, u1, v0 };
    v[2] = (PosColorTexVertex){ x + w, y + h, 0, tint, u1, v1 };
    v[3] = (PosColorTexVertex){ x,     y + h, 0, tint, u0, v1 };

    uint16_t *idx = (uint16_t *)tib.data;
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;

    bgfx_texture_handle_t bgfx_tex;
    bgfx_tex.idx = tex.idx;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);
    bgfx_set_texture(0, sampler, bgfx_tex, UINT32_MAX);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                 | BGFX_STATE_BLEND_ALPHA, 0);
    bgfx_submit(JCE_VIEW_UI, prog, 0, BGFX_DISCARD_ALL);
}
