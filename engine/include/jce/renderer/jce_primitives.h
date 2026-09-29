/*
 * jce_primitives.h  2D primitive drawing via bgfx transient buffers.
 */

#ifndef JCE_PRIMITIVES_H
#define JCE_PRIMITIVES_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;

/* Pack RGBA (0-255 each) into ABGR uint32 for bgfx vertex color. */
JCE_API uint32_t jce_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Draw a single filled rectangle. */
JCE_API void jce_draw_filled_rect(const JceRenderer *r,
                          float x, float y, float w, float h,
                          uint32_t color);

/* Draw a single rectangle outline (4 line segments). */
JCE_API void jce_draw_rect_outline(const JceRenderer *r,
                           float x, float y, float w, float h,
                           uint32_t color);

/* Draw multiple filled rectangles.
   rects: packed float[count*4] = { x, y, w, h, ... }. */
JCE_API void jce_draw_filled_rects(const JceRenderer *r,
                           const float *rects, int count,
                           uint32_t color);

/* Draw multiple rectangle outlines.
   rects: packed float[count*4] = { x, y, w, h, ... }. */
JCE_API void jce_draw_rect_outlines(const JceRenderer *r,
                            const float *rects, int count,
                            uint32_t color);

/* Draw a connected polyline in UI pixel space.
   points_xy: packed float[point_count*2] = { x0, y0, x1, y1, ... }. */
JCE_API void jce_draw_polyline(const JceRenderer *r,
                       const float *points_xy, int point_count,
                       uint32_t color);

/* Draw a textured rectangle.
   tint: color multiplied with texture (use 0xFFFFFFFF for no tint).
   uv: texture coordinate region {u0, v0, u1, v1} or NULL for {0,0,1,1}. */
JCE_API void jce_draw_textured_rect(const JceRenderer *r,
                            float x, float y, float w, float h,
                            JceTexture tex, uint32_t tint,
                            const float *uv);

/* View-targeted variants of the rect draws.  Identical to the functions
   above but submit into an explicit bgfx `view_id` instead of the fixed
   JCE_VIEW_UI overlay, so off-screen UI passes (editor scene/game panels,
   world-space canvases) can draw 2D into their own framebuffer's view.
   The caller must have set that view's ortho transform + rect.  Engine-
   internal (not part of the public consumer API). */
/* A 2D rotate + scale about a pivot, for a rect draw.
 *
 * The pivot is ABSOLUTE, in draw-space pixels, not a fraction of the rect.
 * That is deliberate and it is what makes one transform usable for a whole
 * UI element: a button is an image quad plus a run of glyph quads, and each
 * glyph has its own little rect.  A fraction-of-rect pivot would spin every
 * glyph about its own centre and scatter the word; an absolute pivot spins
 * them all about the element's, which is what "rotate the button" means.
 * The caller resolves it once -- x + w * rect_pivot -- and passes it to
 * every draw the element makes.
 *
 * `angle_deg` turns CLOCKWISE on screen.  Draw space here is y-down, so a
 * mathematically positive rotation appears clockwise, and saying so is
 * cheaper than leaving every caller to discover it.
 *
 * A NULL JceRectXform, or one with angle 0 and scale (1,1), draws the same
 * four vertices the axis-aligned entry points do -- bit for bit, not merely
 * close: the transform is skipped entirely rather than applied as identity. */
typedef struct {
    float pivot[2];     /* ABSOLUTE draw-space pixels */
    float angle_deg;    /* clockwise on screen */
    float scale[2];     /* 1,1 = unscaled */
} JceRectXform;

JCE_API void jce_draw_filled_rect_view(const JceRenderer *r, uint16_t view_id,
                               float x, float y, float w, float h,
                               uint32_t color);

/* The same draws, with a rect transform.  `xf` NULL == the calls above. */
JCE_API void jce_draw_filled_rect_view_xf(const JceRenderer *r, uint16_t view_id,
                               float x, float y, float w, float h,
                               uint32_t color, const JceRectXform *xf);
JCE_API void jce_draw_textured_rect_view_xf(const JceRenderer *r, uint16_t view_id,
                                 float x, float y, float w, float h,
                                 JceTexture tex, uint32_t tint,
                                 const float *uv, const JceRectXform *xf);
JCE_API void jce_draw_textured_rect_view(const JceRenderer *r, uint16_t view_id,
                                 float x, float y, float w, float h,
                                 JceTexture tex, uint32_t tint,
                                 const float *uv);

/* Opaque variant: writes RGBA with blending disabled.  For full-frame
   composite copies (e.g. folding a post-fx output back into an offscreen
   target) where the source alpha channel must not modulate the copy. */
JCE_API void jce_draw_textured_rect_view_opaque(const JceRenderer *r, uint16_t view_id,
                                        float x, float y, float w, float h,
                                        JceTexture tex, uint32_t tint,
                                        const float *uv);

/* ── The 2D scissor every rect draw obeys ────────────────────────────────
 *
 * WHY IT LIVES HERE and not in the caller.  bgfx_set_scissor is PER-DRAW: it
 * is consumed by the next submit and cleared, so a clipped subtree has to
 * re-arm before EVERY submit inside it.  jce_ui_canvas.c learned that and
 * wrapped its own two rect calls in macros that armed it — which clipped
 * quads and left TEXT unclipped, because jce_text_draw_scaled_view submits
 * ONE DRAW PER GLYPH from inside jce_text.c, where the canvas's macros do not
 * reach.  Scrolled text ran outside its viewport, and no amount of care at
 * the call sites could have fixed it.
 *
 * So the duty moves to the file that OWNS the submits: every rect draw below
 * arms this before its own bgfx_submit, and a new draw site cannot forget.
 *
 * WHOLE-PIXEL, in framebuffer coordinates of the view being drawn into.  A
 * zero or negative width or height CLEARS the scissor rather than clipping
 * everything away, because "no clip" is the state a caller means when it has
 * nothing to clip to, and a widget with an empty rect must not silently blank
 * the rest of the frame.
 *
 * NOT saved/restored BY THIS API: it is one global.  A caller with nesting
 * restores it itself -- the UI canvas does, because its clip regions DO nest
 * (a ScrollView inside a ScrollView) and a stack here would be a second place
 * that has to agree with the caller's recursion about what "the current clip"
 * means. */
JCE_API void jce_draw_set_scissor(int x, int y, int w, int h);

/* Clear it.  Equivalent to jce_draw_set_scissor(0, 0, 0, 0), spelled so a
 * reader of the call site does not have to know that. */
JCE_API void jce_draw_clear_scissor(void);

/* The active scissor, if any.  Answers false and touches `out` not at all
 * when none is set, so "not clipped" and "clipped to (0,0,0,0)" are different
 * answers — a pixel comparison cannot tell those apart, and they are the two
 * states a clipping bug lands in. */
JCE_API bool jce_draw_get_scissor(int out_xywh[4]);

JCE_EXTERN_C_END

#endif /* JCE_PRIMITIVES_H */
