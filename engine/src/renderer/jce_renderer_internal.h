/*
 * jce_renderer_internal.h  Renderer-internal accessors.
 *
 * Only for use within the renderer/ module (e.g. jce_primitives.c).
 * External modules should use the public API in jce_renderer.h.
 */

#ifndef JCE_RENDERER_INTERNAL_H
#define JCE_RENDERER_INTERNAL_H

#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_primitives.h>   /* JceRectXform */

#include <bgfx/c99/bgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Renderer-internal accessors (not for use outside renderer/) --- */

/* Color (pos+color) vertex layout and shader program. */
const bgfx_vertex_layout_t *jce_renderer_get_layout(const JceRenderer *r);
bgfx_program_handle_t       jce_renderer_get_program(const JceRenderer *r);

/* Textured (pos+color+uv) vertex layout and shader program. */
const bgfx_vertex_layout_t *jce_renderer_get_layout_textured(const JceRenderer *r);
bgfx_program_handle_t       jce_renderer_get_program_textured(const JceRenderer *r);
/* vs_textured + fs_text_sdf.  UINT16_MAX when the pak has no such
 * program, which is the caller's signal to stay on the bitmap path. */
bgfx_program_handle_t       jce_renderer_get_program_text_sdf(const JceRenderer *r);
JceUniformHandle            jce_renderer_get_sdf_params_uniform(const JceRenderer *r);
JceUniformHandle            jce_renderer_get_sdf_outline_uniform(const JceRenderer *r);
JceUniformHandle            jce_renderer_get_sdf_shadow_offset_uniform(const JceRenderer *r);
JceUniformHandle            jce_renderer_get_sdf_shadow_color_uniform(const JceRenderer *r);

/* ── Batched 2D textured quads ──────────────────────────────────────────
 *
 * One screen-space quad with its own UVs and tint.  All the quads in a call
 * share ONE texture and ONE draw.
 *
 * WHY.  jce_text.c drew each glyph through jce_draw_textured_rect_view, and
 * that function allocates a transient buffer, binds a texture, sets state and
 * submits -- per glyph.  A 60-character line was 60 draw calls that differed
 * only in four floats, and jce_ui_canvas.c:80 said so in its own comment
 * ("submits ONE DRAW PER GLYPH from inside jce_text.c") while the cost stayed.
 *
 * NOT jce_sprite_batch: that one takes a 4x4 world transform per sprite and
 * owns a heap batch object with a begin/add/flush lifetime -- a 3D billboard
 * batcher, checked before this was written and not the same thing as a run
 * of screen-space quads that already know their pixels.
 *
 * Internal, not public: the only caller is jce_text.c, one directory over, and
 * the public 2D API is a rect at a time by design.
 */
typedef struct {
    float    x, y, w, h;      /* screen-space rect */
    float    u0, v0, u1, v1;  /* atlas UVs */
    uint32_t tint;
} JceQuad2D;

/* Alpha-blended, like jce_draw_textured_rect_view.  Runs longer than
 * JCE_QUADS_PER_DRAW are split across draws because indices are 16-bit. */
#define JCE_QUADS_PER_DRAW 16383u

void jce_draw_textured_quads_view(const JceRenderer *r, uint16_t view_id,
                                  const JceQuad2D *quads, uint32_t count,
                                  JceTexture tex);

/* The same batch through ONE transform -- the shape text needs, since a line
 * of text is a run of glyph quads that has to turn together.  `xf` NULL is
 * the call above, unchanged.  Internal for the same reason that one is:
 * JceQuad2D is not part of the consumer API. */
void jce_draw_textured_quads_view_xf(const JceRenderer *r, uint16_t view_id,
                                     const JceQuad2D *quads, uint32_t count,
                                     JceTexture tex, const JceRectXform *xf);

/* Everything the distance-field shader needs beyond the quads themselves.
 *
 * A struct rather than five more parameters, because the next effect should be
 * a field and not another signature -- and because `smoothing` and `edge`
 * belong with the effects that are measured in the same units they are. */
typedef struct {
    float smoothing;        /* half-width of the edge, in DISTANCE units */
    float edge;             /* the distance value that IS the outline */
    float outline_width;    /* same units; 0 = no outline, and no second
                             * threshold is evaluated */
    float outline_rgba[4];
    float shadow_uv[2];     /* offset in TEXTURE space, so the shader never
                             * needs to know the atlas dimensions */
    float shadow_rgba[4];   /* .w == 0 disables it, second sample included */
} JceTextSdfStyle;

/* The same batch, drawn as a DISTANCE FIELD: the atlas holds distance rather
 * than coverage, so the edge is recovered per fragment instead of being
 * whatever bilinear filtering left behind.  `smoothing` is the half-width of
 * the transition in distance units and `edge` is the value that IS the
 * outline -- both come from the text renderer, which is the thing that knows
 * how many screen pixels one atlas texel covers.
 *
 * Returns false and draws NOTHING when the pak has no fs_text_sdf, so a
 * caller can fall back rather than silently losing its text. */
bool jce_draw_text_sdf_quads_view_xf(const JceRenderer *r, uint16_t view_id,
                                     const JceQuad2D *quads, uint32_t count,
                                     JceTexture tex, const JceRectXform *xf,
                                     const JceTextSdfStyle *style);

/* Lighting uniforms (used by jce_lighting.c). */
bgfx_uniform_handle_t       jce_renderer_get_light_dir_uniform(const JceRenderer *r);
bgfx_uniform_handle_t       jce_renderer_get_light_color_uniform(const JceRenderer *r);

/* PBR shader programs (bgfx handles). */
bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr_skinned(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow_skinned(const JceRenderer *r);
bgfx_program_handle_t jce_renderer_get_bgfx_program_terrain(const JceRenderer *r);

/* Render-target scrub helpers (moved out of the public jce_views.h because
 * jce_zero_init_mem returns a bgfx type). No-op stub kept for ABI; the real
 * workhorse returns zeroed bgfx memory for bgfx_create_texture_2d's _mem so
 * a fresh texture starts solid-black instead of leftover VRAM. */
void jce_clear_freshly_created_fbo(unsigned short fb_idx,
                                   unsigned short width,
                                   unsigned short height);
const bgfx_memory_t *jce_zero_init_mem(unsigned int size_bytes);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RENDERER_INTERNAL_H */
