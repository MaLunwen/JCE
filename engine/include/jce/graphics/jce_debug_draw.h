/*
 * jce_debug_draw.h  3D debug line/shape drawing for physics visualization.
 *
 * Accumulates line segments in a transient buffer and flushes them as
 * a single BGFX_STATE_PT_LINES draw call.
 */

#ifndef JCE_DEBUG_DRAW_H
#define JCE_DEBUG_DRAW_H

#include <jce/core/jce_math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;

/* Add a single line segment. */
void jce_debug_draw_line(jce_vec3 from, jce_vec3 to, uint32_t abgr);

/* Draw a wireframe box. */
void jce_debug_draw_box(jce_vec3 center, jce_vec3 half_extents,
                         jce_quat rot, uint32_t abgr);

/* Draw a wireframe sphere (3 circles: XY, XZ, YZ). */
void jce_debug_draw_sphere(jce_vec3 center, float radius, uint32_t abgr);

/* Draw a wireframe capsule (Y-axis oriented). */
void jce_debug_draw_capsule(jce_vec3 center, float radius,
                             float half_height, jce_quat rot,
                             uint32_t abgr);

/* Flush all accumulated lines as a single draw call.
 * Call once per frame after all debug shapes have been queued. */
void jce_debug_draw_flush(uint16_t view_id, const JceRenderer *renderer);

/* Clear all accumulated lines without drawing. */
void jce_debug_draw_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_DEBUG_DRAW_H */
