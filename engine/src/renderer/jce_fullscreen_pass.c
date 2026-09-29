/* jce_fullscreen_pass.c -- see the header for what this is and where it stops. */
#include "renderer/jce_fullscreen_pass.h"

#include <string.h>

typedef struct { float pos[2]; float uv[2]; } JceFsQuadVertex;

void jce_fs_quad_init(JceFsQuad *q)
{
    if (!q) return;
    memset(q, 0, sizeof(*q));
    q->vbh.idx = UINT16_MAX;
    q->ibh.idx = UINT16_MAX;

    /* BGFX_RENDERER_TYPE_NOOP: the layout is renderer-agnostic here, exactly
     * as each of the four copies had it. */
    bgfx_vertex_layout_begin(&q->layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&q->layout, BGFX_ATTRIB_POSITION, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&q->layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&q->layout);

    static const JceFsQuadVertex verts[4] = {
        { { -1.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  1.0f, -1.0f }, { 1.0f, 1.0f } },
        { {  1.0f,  1.0f }, { 1.0f, 0.0f } },
        { { -1.0f,  1.0f }, { 0.0f, 0.0f } },
    };
    static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };

    q->vbh = bgfx_create_vertex_buffer(bgfx_copy(verts, sizeof(verts)),
                                       &q->layout, BGFX_BUFFER_NONE);
    q->ibh = bgfx_create_index_buffer(bgfx_copy(idx, sizeof(idx)),
                                      BGFX_BUFFER_NONE);
}

void jce_fs_quad_destroy(JceFsQuad *q)
{
    if (!q) return;
    if (q->vbh.idx != UINT16_MAX) bgfx_destroy_vertex_buffer(q->vbh);
    if (q->ibh.idx != UINT16_MAX) bgfx_destroy_index_buffer(q->ibh);
    q->vbh.idx = UINT16_MAX;
    q->ibh.idx = UINT16_MAX;
}

void jce_fs_quad_bind(const JceFsQuad *q)
{
    if (!q) return;
    bgfx_set_vertex_buffer(0, q->vbh, 0, 4);
    bgfx_set_index_buffer(q->ibh, 0, 6);
}

void jce_fs_target_init(JceFsTarget *t)
{
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->fb.idx  = UINT16_MAX;
    t->tex.idx = UINT16_MAX;
}

void jce_fs_target_create(JceFsTarget *t, int w, int h,
                          bgfx_texture_format_t format, uint64_t sampler_flags)
{
    if (!t) return;
    if (t->fb.idx != UINT16_MAX) bgfx_destroy_frame_buffer(t->fb);
    t->w      = w > 0 ? w : 1;
    t->h      = h > 0 ? h : 1;
    t->format = format;
    t->flags  = sampler_flags;
    t->fb = bgfx_create_frame_buffer((uint16_t)t->w, (uint16_t)t->h, format,
                                     BGFX_TEXTURE_RT | sampler_flags);
    t->tex = bgfx_get_texture(t->fb, 0);
}

bool jce_fs_target_resize(JceFsTarget *t, int w, int h)
{
    if (!t || w <= 0 || h <= 0) return false;
    if (w == t->w && h == t->h) return false;
    jce_fs_target_create(t, w, h, t->format, t->flags);
    return true;
}

void jce_fs_target_destroy(JceFsTarget *t)
{
    if (!t) return;
    if (t->fb.idx != UINT16_MAX) bgfx_destroy_frame_buffer(t->fb);
    t->fb.idx  = UINT16_MAX;
    t->tex.idx = UINT16_MAX;
}
