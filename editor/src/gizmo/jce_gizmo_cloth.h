/*
 * jce_gizmo_cloth.h  P3-C.4 follow-up  Cloth-gizmo overlay (Scene View).
 *
 * Draws the live cloth as a wireframe grid using horizontal +
 * vertical neighbour lines straight from jce_cloth_get_positions.
 * All shapes are routed through jce_debug_draw_*; this header does
 * not introduce a new render path.
 */

#ifndef JCE_GIZMO_CLOTH_H
#define JCE_GIZMO_CLOTH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/scene/jce_scene.h>

void jce_gizmo_cloth_draw_from_component(const JceClothComponent *cloth);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GIZMO_CLOTH_H */
