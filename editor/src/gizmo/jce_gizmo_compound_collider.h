/*
 * jce_gizmo_compound_collider.h  Compound-collider Scene View overlay.
 *
 * Draws the cooked per-object child shapes (box / sphere / capsule wire,
 * convex-hull bounds, triangle-mesh edges) for a selected entity that
 * owns a JceCompoundColliderComponent, each child in its own palette
 * colour so the per-object split is visible at a glance.
 *
 * The cook is expensive, so the model-space wireframe is cached by
 * model path + cook settings and only rebuilt when those change; each
 * frame the cached segments are transformed by the entity's TRS and
 * emitted through jce_debug_draw_line — no new render path.
 */

#ifndef JCE_GIZMO_COMPOUND_COLLIDER_H
#define JCE_GIZMO_COMPOUND_COLLIDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/scene/jce_scene.h>

/* override_abgr: 0 = draw each child in its own palette colour; non-zero =
 * force every segment to this ABGR colour (so the debug overlay can draw all
 * compound colliders one colour, and the selection pass can draw the selected
 * one in the selection colour instead of fighting it with palette green). */
/* detailed: 1 = fitted triangle wireframe (use for ONE selected prop);
 *           0 = outer bounds box per child (use for the all-props overlay). */
void jce_gizmo_compound_collider_draw_from_component(
    JceScene *scene, JceEntity owner, const JceCompoundColliderComponent *cc,
    unsigned int override_abgr, int detailed);

/* Drop cached wireframes (call when assets change / on shutdown). */
void jce_gizmo_compound_collider_clear_cache(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GIZMO_COMPOUND_COLLIDER_H */
