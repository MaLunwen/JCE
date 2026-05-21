/*
 * jce_gizmo_joint.h  P3-C.6  Joint-gizmo overlay (Scene View).
 *
 * Renders anchor spheres, connection line, primary axis, and limit
 * geometry (hinge arc / slider segment / 6DOF box + arcs) for a
 * physics joint.  All shapes go through jce_debug_draw_*; this header
 * adds no new render path.
 *
 * Two callable shapes:
 *   - jce_gizmo_joint_draw_from_info() — back-end agnostic; caller
 *     resolves a JcePhysicsJointInfo (e.g. via
 *     jce_physics_joint_get_info during Play).
 *   - jce_gizmo_joint_draw_from_component() — edit-time helper that
 *     reads the authoring JceConstraintComponent + the owner/target
 *     transforms straight from the scene, so the gizmo is visible
 *     without a live physics world.
 */

#ifndef JCE_GIZMO_JOINT_H
#define JCE_GIZMO_JOINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/physics/jce_physics_joint_query.h>
#include <jce/middleware/scene/jce_scene.h>

void jce_gizmo_joint_draw_from_info(const JcePhysicsJointInfo *info);

void jce_gizmo_joint_draw_from_component(JceScene *scene,
                                         JceEntity owner,
                                         const JceConstraintComponent *con);

#ifdef __cplusplus
}
#endif

#endif /* JCE_GIZMO_JOINT_H */
