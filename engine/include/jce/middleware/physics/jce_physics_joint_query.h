/*
 * jce_physics_joint_query.h  Joint / constraint introspection (P3-C.6).
 *
 * Lets the editor (or any tool) query the world-space anchors, axis,
 * and limit range of a live constraint so it can render a joint
 * gizmo in Scene View.  Kept additive — no existing physics call
 * changes shape.
 *
 * Layer: L4 (middleware/physics).  Read-only; safe between steps.
 */

#ifndef JCE_PHYSICS_JOINT_QUERY_H
#define JCE_PHYSICS_JOINT_QUERY_H


#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePhysicsWorld JcePhysicsWorld;

/* Joint family — independent of JceConstraintType so the editor can use
 * the query without pulling Bullet-specific enums.  Currently 1:1, but
 * future split (e.g. CONE_TWIST, BALL) stays additive here. */
typedef enum JcePhysicsJointKind {
    JCE_PHYSICS_JOINT_NONE  = 0,
    JCE_PHYSICS_JOINT_BALL  = 1,  /* point-to-point (no axis, no limit) */
    JCE_PHYSICS_JOINT_HINGE = 2,
    JCE_PHYSICS_JOINT_SLIDER = 3,
    JCE_PHYSICS_JOINT_6DOF  = 4
} JcePhysicsJointKind;

/* Resolved joint info — all vectors in world space, axis pre-normalised.
 * For HINGE / SLIDER the scalar limits drive limit_low / limit_high
 * (radians for hinge, metres for slider).  For 6DOF the per-axis
 * linear_* / angular_* arrays drive the bounding-box / arc visuals.
 *
 * body_b.idx == UINT32_MAX when the constraint anchors body A to the
 * world; in that case anchor_b is still meaningful (= world point). */
typedef struct JcePhysicsJointInfo {
    JcePhysicsJointKind kind;
    JceBodyHandle       body_a;
    JceBodyHandle       body_b;
    jce_vec3            anchor_a;        /* world space */
    jce_vec3            anchor_b;        /* world space */
    jce_vec3            axis;            /* world space, normalised */
    float               limit_low;
    float               limit_high;
    jce_vec3            linear_lower;    /* 6DOF only — local A frame */
    jce_vec3            linear_upper;
    jce_vec3            angular_lower;
    jce_vec3            angular_upper;
} JcePhysicsJointInfo;

/* Populate `info` for the first constraint attached to `body`.
 *
 * v1 limitation: a body may participate in several constraints; this
 * call returns the lowest-index match only.  Returns false (and leaves
 * `info` untouched) when the body owns no constraint, the world or
 * handle is invalid, or `info` is NULL.
 *
 * Safe to call between physics steps from the same thread that owns
 * the world.  Read-only — does not mutate the simulation. */
JCE_API bool JCE_CALL jce_physics_joint_get_info(const JcePhysicsWorld *world,
                                                 JceBodyHandle body,
                                                 JcePhysicsJointInfo *info);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_JOINT_QUERY_H */
