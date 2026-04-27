/*
 * jce_primitives.h  2D primitive drawing via bgfx transient buffers.
 */

#ifndef JCE_PRIMITIVES_H
#define JCE_PRIMITIVES_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;

/* Pack RGBA (0-255 each) into ABGR uint32 for bgfx vertex color. */
JCE_API uint32_t jce_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Draw a single filled rectangle. */
void jce_draw_filled_rect(const JceRenderer *r,
                          float x, float y, float w, float h,
                          uint32_t color);

/* Draw a single rectangle outline (4 line segments). */
void jce_draw_rect_outline(const JceRenderer *r,
                           float x, float y, float w, float h,
                           uint32_t color);

/* Draw multiple filled rectangles.
   rects: packed float[count*4] = { x, y, w, h, ... }. */
void jce_draw_filled_rects(const JceRenderer *r,
                           const float *rects, int count,
                           uint32_t color);

/* Draw multiple rectangle outlines.
   rects: packed float[count*4] = { x, y, w, h, ... }. */
void jce_draw_rect_outlines(const JceRenderer *r,
                            const float *rects, int count,
                            uint32_t color);

/* Draw a connected polyline in UI pixel space.
   points_xy: packed float[point_count*2] = { x0, y0, x1, y1, ... }. */
void jce_draw_polyline(const JceRenderer *r,
                       const float *points_xy, int point_count,
                       uint32_t color);

/* Draw a textured rectangle.
   tint: color multiplied with texture (use 0xFFFFFFFF for no tint).
   uv: texture coordinate region {u0, v0, u1, v1} or NULL for {0,0,1,1}. */
void jce_draw_textured_rect(const JceRenderer *r,
                            float x, float y, float w, float h,
                            JceTexture tex, uint32_t tint,
                            const float *uv);

JCE_EXTERN_C_END

#endif /* JCE_PRIMITIVES_H */
