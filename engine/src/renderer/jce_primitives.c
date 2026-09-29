/*
 * jce_primitives.c  2D primitive drawing implementation.
 */

#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_views.h>

#include "jce_renderer_internal.h"
#include "jce_primitives_selftest.h"

#include <jce/os/core/jce_log.h>

/* cosf / sinf, for the rect transform's rotation.  This was MISSING and the
 * tree did not notice, because MSVC recognises both as intrinsics under /Oi
 * -- which /O2 implies -- so the release configuration supplies a declaration
 * the source never asked for.  The debug configuration does not, and there
 * `cosf` went through an implicit `extern int`: the CRT returns a float in
 * XMM0 and the caller read EAX, so a rotated primitive got a garbage basis.
 * Release-correct and debug-broken, from one absent include. */
#include <math.h>
#include <string.h>

#define LOG_TAG "primitives"

/* ── The 2D scissor.  See jce_primitives.h for why it lives here. ────────
 *
 * Armed immediately before every bgfx_submit in this file, because
 * bgfx_set_scissor is consumed by the next submit and then cleared -- a
 * single arm before a multi-draw call (a glyph run, say) would clip the
 * first glyph and no others. */
static bool s_scissor_on;
static int  s_scissor[4];

void jce_draw_set_scissor(int x, int y, int w, int h)
{
    /* Clamp first, THEN one emptiness check.  There used to be a check before
     * the clamp too; it was redundant and provably so -- a negative origin
     * only ever SHRINKS the extent (w += x with x < 0), so anything the first
     * check would have caught the second catches as well.  A mutation that
     * deleted it changed no test result, which is how it was found: a guard
     * that cannot be the one that matters is a guard that will be maintained
     * forever for nothing. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) { s_scissor_on = false; return; }
    s_scissor[0] = x; s_scissor[1] = y;
    s_scissor[2] = w; s_scissor[3] = h;
    s_scissor_on = true;
}

void jce_draw_clear_scissor(void) { s_scissor_on = false; }

bool jce_draw_get_scissor(int out_xywh[4])
{
    if (!s_scissor_on) return false;
    if (out_xywh) {
        out_xywh[0] = s_scissor[0]; out_xywh[1] = s_scissor[1];
        out_xywh[2] = s_scissor[2]; out_xywh[3] = s_scissor[3];
    }
    return true;
}

/* Call IMMEDIATELY before a bgfx_submit and nowhere else. */
static void jce_scissor_arm(void)
{
    if (!s_scissor_on) return;
    bgfx_set_scissor((uint16_t)s_scissor[0], (uint16_t)s_scissor[1],
                     (uint16_t)s_scissor[2], (uint16_t)s_scissor[3]);
}

typedef struct { float x, y, z; uint32_t abgr; }             PosColorVertex;
typedef struct { float x, y, z; uint32_t abgr; float u, v; } PosColorTexVertex;

uint32_t jce_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16)
         | ((uint32_t)g <<  8) |  (uint32_t)r;
}

/* The four corners of a rect (x0,y0, x1,y1, ...), with an optional 2D
 * rotate + scale about a pivot.  ONE place computes them, so a transformed
 * quad and an axis-aligned one cannot disagree about where the rect is. */
static void prim_rect_corners(float x, float y, float w, float h,
                              const JceRectXform *xf, float out[8])
{
    /* Exactly the vertices the axis-aligned path has always emitted.  Written
     * first and returned unchanged when there is nothing to apply, so "no
     * transform" is byte-identical rather than identity-multiplied. */
    out[0] = x;     out[1] = y;
    out[2] = x + w; out[3] = y;
    out[4] = x + w; out[5] = y + h;
    out[6] = x;     out[7] = y + h;
    if (!xf) return;

    float sx = xf->scale[0], sy = xf->scale[1];
    /* A scale of exactly 0 is not a size, it is a mistake that draws nothing
     * and looks like the element failing to exist.  Same rule the material UV
     * transform uses for a zero tiling, and for the same reason: an unset
     * field in an older asset is indistinguishable from a typed 0. */
    if (sx == 0.0f) sx = 1.0f;
    if (sy == 0.0f) sy = 1.0f;
    if (xf->angle_deg == 0.0f && sx == 1.0f && sy == 1.0f) return;

    const float a  = xf->angle_deg * 0.01745329252f;
    /* ABSOLUTE pivot -- see the header.  A rect-relative one would spin every
     * glyph of a label about its own centre instead of about the label. */
    const float px = xf->pivot[0];
    const float py = xf->pivot[1];
    const float ca = cosf(a), sa = sinf(a);
    for (int i = 0; i < 4; i++) {
        const float dx = (out[i * 2 + 0] - px) * sx;
        const float dy = (out[i * 2 + 1] - py) * sy;
        out[i * 2 + 0] = px + dx * ca - dy * sa;
        out[i * 2 + 1] = py + dx * sa + dy * ca;
    }
}

void jce_draw_filled_rect_view(const JceRenderer *r, uint16_t view_id,
                               float x, float y, float w, float h,
                               uint32_t color)
{
    jce_draw_filled_rect_view_xf(r, view_id, x, y, w, h, color, NULL);
}

void jce_draw_filled_rect_view_xf(const JceRenderer *r, uint16_t view_id,
                               float x, float y, float w, float h,
                               uint32_t color, const JceRectXform *xf)
{
    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout(r);
    bgfx_program_handle_t prog = jce_renderer_get_program(r);
    if (!layout) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;

    if (!bgfx_alloc_transient_buffers(&tvb, layout, 4, &tib, 6, false))
        return;

    float c4[8];
    prim_rect_corners(x, y, w, h, xf, c4);
    PosColorVertex *v = (PosColorVertex *)tvb.data;
    v[0] = (PosColorVertex){ c4[0], c4[1], 0, color };
    v[1] = (PosColorVertex){ c4[2], c4[3], 0, color };
    v[2] = (PosColorVertex){ c4[4], c4[5], 0, color };
    v[3] = (PosColorVertex){ c4[6], c4[7], 0, color };

    uint16_t *idx = (uint16_t *)tib.data;
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                 | BGFX_STATE_BLEND_ALPHA, 0);
    jce_scissor_arm();
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_draw_filled_rect(const JceRenderer *r,
                          float x, float y, float w, float h,
                          uint32_t color)
{
    jce_draw_filled_rect_view(r, JCE_VIEW_UI, x, y, w, h, color);
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
    jce_scissor_arm();
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
    jce_scissor_arm();
    bgfx_submit(JCE_VIEW_UI, prog, 0, BGFX_DISCARD_ALL);
}

/* Write `n` quads as 4n vertices and 6n indices into caller memory.
 *
 * Pulled out of the submit path so it can be checked without a GPU: this is
 * the whole of what batching changed, and the one way it goes wrong -- quad
 * k's indices addressing quad k-1's vertices -- yields text that is subtly
 * wrong rather than absent.  See jce_primitives_self_test below.
 */
static void fill_quads(PosColorTexVertex *v, uint16_t *idx,
                       const JceQuad2D *q, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        const JceQuad2D *s = &q[i];
        const uint16_t   b = (uint16_t)(i * 4u);
        v[b + 0] = (PosColorTexVertex){ s->x,        s->y,
                                        0, s->tint, s->u0, s->v0 };
        v[b + 1] = (PosColorTexVertex){ s->x + s->w, s->y,
                                        0, s->tint, s->u1, s->v0 };
        v[b + 2] = (PosColorTexVertex){ s->x + s->w, s->y + s->h,
                                        0, s->tint, s->u1, s->v1 };
        v[b + 3] = (PosColorTexVertex){ s->x,        s->y + s->h,
                                        0, s->tint, s->u0, s->v1 };
        idx[i * 6u + 0] = (uint16_t)(b + 0);
        idx[i * 6u + 1] = (uint16_t)(b + 1);
        idx[i * 6u + 2] = (uint16_t)(b + 2);
        idx[i * 6u + 3] = (uint16_t)(b + 0);
        idx[i * 6u + 4] = (uint16_t)(b + 2);
        idx[i * 6u + 5] = (uint16_t)(b + 3);
    }
}

/* Shared body for every textured 2D draw in this file.
 *
 * The blended rect and the opaque rect were full copies of each other -- same
 * alloc, same four vertices, same six indices, same bind -- differing in the
 * one `state` word.  They are now the same code, and it takes a RUN of quads
 * rather than one, because the caller that dominates this path is a glyph
 * loop.
 *
 * Order is preserved for free: a run shares one texture and one state, so the
 * quads inside it can only overlap each other, and the UI views are
 * BGFX_VIEW_MODE_SEQUENTIAL, meaning a batch occupies exactly the position in
 * the submit order its first quad used to.  Nothing else can be submitted
 * between the members of a run, because the run is built and drained inside a
 * single call.
 */
static void draw_textured_quads(const JceRenderer *r, uint16_t view_id,
                                const JceQuad2D *q, uint32_t count,
                                JceTexture tex, uint64_t state)
{
    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout_textured(r);
    bgfx_program_handle_t prog = jce_renderer_get_program_textured(r);
    JceUniformHandle uh = jce_renderer_get_tex_uniform(r);
    bgfx_uniform_handle_t sampler = { uh.idx };
    bgfx_texture_handle_t bgfx_tex;

    if (!layout || prog.idx == UINT16_MAX || !q || count == 0) return;
    bgfx_tex.idx = tex.idx;

    while (count > 0) {
        const uint32_t n = (count < JCE_QUADS_PER_DRAW) ? count
                                                        : JCE_QUADS_PER_DRAW;
        bgfx_transient_vertex_buffer_t tvb;
        bgfx_transient_index_buffer_t  tib;

        /* A pool that cannot fit this chunk will not fit the next one either,
         * so stop rather than spin -- and stop having drawn the chunks that
         * did fit, which is what the per-rect version did too. */
        if (!bgfx_alloc_transient_buffers(&tvb, layout, n * 4u,
                                          &tib, n * 6u, false))
            return;

        fill_quads((PosColorTexVertex *)tvb.data, (uint16_t *)tib.data, q, n);

        bgfx_set_transient_vertex_buffer(0, &tvb, 0, n * 4u);
        bgfx_set_transient_index_buffer(&tib, 0, n * 6u);
        bgfx_set_texture(0, sampler, bgfx_tex, UINT32_MAX);
        bgfx_set_state(state, 0);
        jce_scissor_arm();
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);

        q += n;
        count -= n;
    }
}

/* The batched fill, with a transform.  Vertices are written by the same
 * corner routine the single-rect path uses, so a rotated label and a rotated
 * panel cannot disagree about what the angle means. */
static void fill_quads_xf(PosColorTexVertex *v, uint16_t *idx,
                          const JceQuad2D *q, uint32_t n,
                          const JceRectXform *xf)
{
    for (uint32_t i = 0; i < n; i++) {
        const JceQuad2D *s = &q[i];
        const uint16_t   b = (uint16_t)(i * 4u);
        float c4[8];
        prim_rect_corners(s->x, s->y, s->w, s->h, xf, c4);
        v[b + 0] = (PosColorTexVertex){ c4[0], c4[1], 0, s->tint, s->u0, s->v0 };
        v[b + 1] = (PosColorTexVertex){ c4[2], c4[3], 0, s->tint, s->u1, s->v0 };
        v[b + 2] = (PosColorTexVertex){ c4[4], c4[5], 0, s->tint, s->u1, s->v1 };
        v[b + 3] = (PosColorTexVertex){ c4[6], c4[7], 0, s->tint, s->u0, s->v1 };
        idx[i * 6u + 0] = (uint16_t)(b + 0);
        idx[i * 6u + 1] = (uint16_t)(b + 1);
        idx[i * 6u + 2] = (uint16_t)(b + 2);
        idx[i * 6u + 3] = (uint16_t)(b + 0);
        idx[i * 6u + 4] = (uint16_t)(b + 2);
        idx[i * 6u + 5] = (uint16_t)(b + 3);
    }
}


static JceQuad2D quad_from_rect(float x, float y, float w, float h,
                                uint32_t tint, const float *uv)
{
    JceQuad2D q;
    q.x = x; q.y = y; q.w = w; q.h = h;
    q.u0 = 0.0f; q.v0 = 0.0f; q.u1 = 1.0f; q.v1 = 1.0f;
    if (uv) { q.u0 = uv[0]; q.v0 = uv[1]; q.u1 = uv[2]; q.v1 = uv[3]; }
    q.tint = tint;
    return q;
}

#define JCE_TEXTURED_2D_BLENDED (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A                                | BGFX_STATE_BLEND_ALPHA)

void jce_draw_textured_quads_view(const JceRenderer *r, uint16_t view_id,
                                  const JceQuad2D *quads, uint32_t count,
                                  JceTexture tex)
{
    draw_textured_quads(r, view_id, quads, count, tex,
                        JCE_TEXTURED_2D_BLENDED);
}

/* ONE submit loop for both programs.  `sdf` NULL is the ordinary textured
 * batch; non-NULL points at the vec4 the distance-field shader reads.  The
 * alternative -- a second copy of this loop -- is how the SDF path would end
 * up blending or scissoring differently from the bitmap path without anyone
 * deciding that it should. */
static void draw_quads_xf_prog(const JceRenderer *r, uint16_t view_id,
                               const JceQuad2D *quads, uint32_t count,
                               JceTexture tex, const JceRectXform *xf,
                               bgfx_program_handle_t prog,
                               const JceTextSdfStyle *sdf)
{
    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout_textured(r);
    JceUniformHandle uh = jce_renderer_get_tex_uniform(r);
    bgfx_uniform_handle_t sampler = { uh.idx };
    JceUniformHandle sh  = jce_renderer_get_sdf_params_uniform(r);
    JceUniformHandle soh = jce_renderer_get_sdf_outline_uniform(r);
    JceUniformHandle sfh = jce_renderer_get_sdf_shadow_offset_uniform(r);
    JceUniformHandle sch = jce_renderer_get_sdf_shadow_color_uniform(r);
    bgfx_uniform_handle_t sdf_u = { sh.idx };
    bgfx_uniform_handle_t out_u = { soh.idx };
    bgfx_uniform_handle_t sof_u = { sfh.idx };
    bgfx_uniform_handle_t scl_u = { sch.idx };
    bgfx_texture_handle_t bgfx_tex;
    if (!layout || prog.idx == UINT16_MAX || !quads || count == 0) return;
    bgfx_tex.idx = tex.idx;

    while (count > 0) {
        const uint32_t n = (count < JCE_QUADS_PER_DRAW) ? count
                                                        : JCE_QUADS_PER_DRAW;
        bgfx_transient_vertex_buffer_t tvb;
        bgfx_transient_index_buffer_t  tib;
        if (!bgfx_alloc_transient_buffers(&tvb, layout, n * 4u,
                                          &tib, n * 6u, false))
            return;
        if (xf)
            fill_quads_xf((PosColorTexVertex *)tvb.data, (uint16_t *)tib.data,
                          quads, n, xf);
        else
            fill_quads((PosColorTexVertex *)tvb.data, (uint16_t *)tib.data,
                       quads, n);
        bgfx_set_transient_vertex_buffer(0, &tvb, 0, n * 4u);
        bgfx_set_transient_index_buffer(&tib, 0, n * 6u);
        bgfx_set_texture(0, sampler, bgfx_tex, UINT32_MAX);
        if (sdf) {
            /* All four every submit, not just the ones in use: bgfx records
             * only the uniform updates issued since the last state-discarding
             * submit, so a draw that skipped one would inherit the previously
             * sorted draw's value -- which is how a plain label ends up
             * wearing the outline of whatever was drawn before it. */
            const float p[4] = { sdf->smoothing, sdf->edge,
                                 sdf->outline_width, 0.0f };
            const float o[4] = { sdf->outline_rgba[0], sdf->outline_rgba[1],
                                 sdf->outline_rgba[2], sdf->outline_rgba[3] };
            const float f[4] = { sdf->shadow_uv[0], sdf->shadow_uv[1],
                                 0.0f, 0.0f };
            const float c[4] = { sdf->shadow_rgba[0], sdf->shadow_rgba[1],
                                 sdf->shadow_rgba[2], sdf->shadow_rgba[3] };
            if (sdf_u.idx != UINT16_MAX) bgfx_set_uniform(sdf_u, p, 1);
            if (out_u.idx != UINT16_MAX) bgfx_set_uniform(out_u, o, 1);
            if (sof_u.idx != UINT16_MAX) bgfx_set_uniform(sof_u, f, 1);
            if (scl_u.idx != UINT16_MAX) bgfx_set_uniform(scl_u, c, 1);
        }
        bgfx_set_state(JCE_TEXTURED_2D_BLENDED, 0);
        jce_scissor_arm();
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
        quads += n;
        count -= n;
    }
}

void jce_draw_textured_quads_view_xf(const JceRenderer *r, uint16_t view_id,
                                     const JceQuad2D *quads, uint32_t count,
                                     JceTexture tex, const JceRectXform *xf)
{
    if (!xf) {
        jce_draw_textured_quads_view(r, view_id, quads, count, tex);
        return;
    }
    draw_quads_xf_prog(r, view_id, quads, count, tex, xf,
                       jce_renderer_get_program_textured(r), NULL);
}

bool jce_draw_text_sdf_quads_view_xf(const JceRenderer *r, uint16_t view_id,
                                     const JceQuad2D *quads, uint32_t count,
                                     JceTexture tex, const JceRectXform *xf,
                                     const JceTextSdfStyle *style)
{
    const bgfx_program_handle_t prog = jce_renderer_get_program_text_sdf(r);
    /* Report rather than draw the bitmap program with SDF texels in it: that
     * would put a grey halo on every glyph and look like a font bug. */
    if (prog.idx == UINT16_MAX || !style) return false;

    draw_quads_xf_prog(r, view_id, quads, count, tex, xf, prog, style);
    return true;
}

void jce_draw_textured_rect_view(const JceRenderer *r, uint16_t view_id,
                                 float x, float y, float w, float h,
                                 JceTexture tex, uint32_t tint,
                                 const float *uv)
{
    const JceQuad2D q = quad_from_rect(x, y, w, h, tint, uv);
    draw_textured_quads(r, view_id, &q, 1, tex, JCE_TEXTURED_2D_BLENDED);
}

void jce_draw_textured_rect_view_xf(const JceRenderer *r, uint16_t view_id,
                                    float x, float y, float w, float h,
                                    JceTexture tex, uint32_t tint,
                                    const float *uv, const JceRectXform *xf)
{
    /* No transform -> the batched path, unchanged.  JceQuad2D is
     * axis-aligned by construction and it is a PUBLIC batching type; a
     * rotation field on it would change what every batch means, to serve the
     * one quad in a hundred that turns. */
    if (!xf) {
        jce_draw_textured_rect_view(r, view_id, x, y, w, h, tex, tint, uv);
        return;
    }

    const bgfx_vertex_layout_t *layout = jce_renderer_get_layout_textured(r);
    bgfx_program_handle_t prog = jce_renderer_get_program_textured(r);
    JceUniformHandle uh = jce_renderer_get_tex_uniform(r);
    bgfx_uniform_handle_t sampler = { uh.idx };
    bgfx_texture_handle_t bgfx_tex;
    if (!layout || prog.idx == UINT16_MAX) return;
    bgfx_tex.idx = tex.idx;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, layout, 4, &tib, 6, false)) return;

    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (uv) { u0 = uv[0]; v0 = uv[1]; u1 = uv[2]; v1 = uv[3]; }

    float c4[8];
    prim_rect_corners(x, y, w, h, xf, c4);
    PosColorTexVertex *v = (PosColorTexVertex *)tvb.data;
    v[0] = (PosColorTexVertex){ c4[0], c4[1], 0, tint, u0, v0 };
    v[1] = (PosColorTexVertex){ c4[2], c4[3], 0, tint, u1, v0 };
    v[2] = (PosColorTexVertex){ c4[4], c4[5], 0, tint, u1, v1 };
    v[3] = (PosColorTexVertex){ c4[6], c4[7], 0, tint, u0, v1 };

    uint16_t *idx = (uint16_t *)tib.data;
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 0; idx[4] = 2; idx[5] = 3;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, 4);
    bgfx_set_transient_index_buffer(&tib, 0, 6);
    bgfx_set_texture(0, sampler, bgfx_tex, UINT32_MAX);
    bgfx_set_state(JCE_TEXTURED_2D_BLENDED, 0);
    jce_scissor_arm();
    bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
}

void jce_draw_textured_rect_view_opaque(const JceRenderer *r, uint16_t view_id,
                                        float x, float y, float w, float h,
                                        JceTexture tex, uint32_t tint,
                                        const float *uv)
{
    const JceQuad2D q = quad_from_rect(x, y, w, h, tint, uv);
    draw_textured_quads(r, view_id, &q, 1, tex,
                        BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
}

void jce_draw_textured_rect(const JceRenderer *r,
                            float x, float y, float w, float h,
                            JceTexture tex, uint32_t tint,
                            const float *uv)
{
    jce_draw_textured_rect_view(r, JCE_VIEW_UI, x, y, w, h, tex, tint, uv);
}

/* ── Self-test ──────────────────────────────────────────────────────────
 *
 * WHY.  jce_text.c submitted one draw call per glyph until 2026-09-01, so a
 * 60-character line was 60 transient allocations, 60 texture binds, 60 state
 * sets and 60 submits differing only in four floats and a UV rect.  Batching
 * a glyph run into one draw moved the risk from "does it draw" to "is quad k
 * built from quad k's numbers", and that failure mode does not look like a
 * bug in a screenshot: the text is there, in the right font at the right
 * size, with some glyphs doubled and others missing.  Arithmetic settles it;
 * a look does not.
 *
 * Runs headless.  fill_quads touches no bgfx state, which is why it was
 * pulled out of the submit path.
 */
static bool st_eqf(float a, float b) { return (a - b) < 1e-6f && (b - a) < 1e-6f; }

bool jce_primitives_self_test(void)
{
    enum { N = 3 };
    static const uint16_t WANT_IDX[6] = { 0u, 1u, 2u, 0u, 2u, 3u };

    PosColorTexVertex v[N * 4];
    uint16_t          idx[N * 6];
    JceQuad2D         q[N];
    bool     ok = true;
    uint32_t i, j;

    /* Distinct values in every field of every quad.  A batcher that reused
     * quad 0's tint or UVs for the whole run -- the shape of bug that leaves
     * monochrome text looking perfect -- has to disagree with one of these. */
    for (i = 0; i < N; i++) {
        q[i].x  = 10.0f * (float)(i + 1u);
        q[i].y  = 20.0f * (float)(i + 1u);
        q[i].w  =  3.0f + (float)i;
        q[i].h  =  7.0f + (float)i;
        q[i].u0 = 0.10f * (float)i;
        q[i].v0 = 0.20f * (float)i;
        q[i].u1 = q[i].u0 + 0.05f;
        q[i].v1 = q[i].v0 + 0.06f;
        q[i].tint = 0xFF000000u | (uint32_t)(i + 1u);
    }

    /* Poison first: a field the batcher never writes must not read as a
     * plausible zero.  This project has had a gate pass because unwritten
     * memory happened to hold the expected value. */
    memset(v,   0xCD, sizeof v);
    memset(idx, 0xCD, sizeof idx);

    fill_quads(v, idx, q, N);

    for (i = 0; i < N; i++) {
        const uint16_t b  = (uint16_t)(i * 4u);
        const JceQuad2D *s = &q[i];

        /* Corner order, matching what the single-rect path emitted before
         * batching existed: TL, TR, BR, BL. */
        if (!st_eqf(v[b + 0].x, s->x)        || !st_eqf(v[b + 0].y, s->y) ||
            !st_eqf(v[b + 1].x, s->x + s->w) || !st_eqf(v[b + 1].y, s->y) ||
            !st_eqf(v[b + 2].x, s->x + s->w) || !st_eqf(v[b + 2].y, s->y + s->h) ||
            !st_eqf(v[b + 3].x, s->x)        || !st_eqf(v[b + 3].y, s->y + s->h)) {
            LOG_ERROR(LOG_TAG, "self-test: quad %u corner order wrong", i);
            ok = false;
        }

        /* UVs and tint must come from THIS quad. */
        if (!st_eqf(v[b + 0].u, s->u0) || !st_eqf(v[b + 0].v, s->v0) ||
            !st_eqf(v[b + 2].u, s->u1) || !st_eqf(v[b + 2].v, s->v1)) {
            LOG_ERROR(LOG_TAG, "self-test: quad %u UVs wrong", i);
            ok = false;
        }
        for (j = 0; j < 4u; j++) {
            if (v[b + j].abgr != s->tint) {
                LOG_ERROR(LOG_TAG, "self-test: quad %u vertex %u tint "
                          "0x%08X (want 0x%08X)", i, j,
                          v[b + j].abgr, s->tint);
                ok = false;
            }
        }

        /* THE regression this test exists for.  Every index of quad i must
         * address quad i's own four vertices, and form the two triangles the
         * unbatched path formed. */
        for (j = 0; j < 6u; j++) {
            const uint16_t got = idx[i * 6u + j];
            if (got != (uint16_t)(b + WANT_IDX[j])) {
                LOG_ERROR(LOG_TAG, "self-test: quad %u index %u = %u "
                          "(want %u) -- a batched glyph run would draw the "
                          "wrong glyph here", i, j, got,
                          (unsigned)(b + WANT_IDX[j]));
                ok = false;
            }
        }
    }

    /* n == 0 must write nothing at all.  jce_text.c guards its own flush,
     * so this is the belt to that braces: the guard and the batcher would
     * have to fail together, and only one of them is in this file. */
    memset(v, 0xCD, sizeof v);
    fill_quads(v, idx, q, 0u);
    for (j = 0; j < sizeof v; j++) {
        if (((const unsigned char *)v)[j] != 0xCDu) {
            LOG_ERROR(LOG_TAG, "self-test: count=0 wrote to vertex byte %u",
                      j);
            ok = false;
            break;
        }
    }

    return ok;
}
