/*
 * jce_physics2d.h  2D rigid-body physics (Box2D 3.x backend).
 *
 * Thin wrapper around Box2D v3 for 2D games and UI physics.
 * Registered as an optional subsystem (priority 101).
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 *
 * Layer: Physics (Layer 3 — optional subsystem).
 */

#ifndef JCE_PHYSICS2D_H
#define JCE_PHYSICS2D_H


#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* World lifecycle                                                     */
/* ================================================================== */

typedef struct JcePhysics2D JcePhysics2D;

typedef struct {
    jce_vec2 gravity;          /* default: (0, -9.81) */
    uint32_t max_bodies;       /* default: 4096 */
} JcePhysics2DDesc;

JCE_API JcePhysics2D *jce_physics2d_create(const JcePhysics2DDesc *desc);
JCE_API void          jce_physics2d_destroy(JcePhysics2D *world);

/* ================================================================== */
/* Simulation                                                          */
/* ================================================================== */

/* Step the 2D simulation forward by dt seconds. */
JCE_API void jce_physics2d_step(JcePhysics2D *world, float dt);

/* ================================================================== */
/* 2D body shapes                                                      */
/* ================================================================== */

typedef enum {
    JCE_SHAPE2D_BOX     = 0,
    JCE_SHAPE2D_CIRCLE  = 1,
    JCE_SHAPE2D_CAPSULE = 2,
    JCE_SHAPE2D_SEGMENT = 3
} JceShape2DType;

/* ================================================================== */
/* 2D rigid bodies                                                     */
/* ================================================================== */

typedef struct {
    JceBodyType    type;
    JceShape2DType shape;
    jce_vec2       position;
    float          angle;           /* radians */
    jce_vec2       half_extents;    /* box: half-size; circle: (radius,0);
                                       capsule: (radius, half_length) */
    float          mass;            /* 0 = static */
    float          friction;        /* default: 0.5 */
    float          restitution;     /* default: 0.0 */
    float          linear_damping;
    float          angular_damping;
    bool           fixed_rotation;
} JceBody2DDesc;

JCE_API JceBodyHandle jce_physics2d_body_create(JcePhysics2D *world, const JceBody2DDesc *desc);
JCE_API void          jce_physics2d_body_destroy(JcePhysics2D *world, JceBodyHandle body);

/* Create a body with NO shapes attached.  Callers compose multi-shape
 * bodies (e.g. tilemap colliders) by attaching boxes afterwards with
 * jce_physics2d_body_add_box(). */
JCE_API JceBodyHandle jce_physics2d_body_create_empty(JcePhysics2D *world,
                                                      jce_vec2 pos, float angle,
                                                      JceBodyType type);

/* Attach one axis-aligned box shape (offset in body-local space) to an
 * existing body.  `sensor` shapes detect overlaps without collision
 * response.  Returns false on invalid world/body/extents. */
JCE_API bool          jce_physics2d_body_add_box(JcePhysics2D *world,
                                                 JceBodyHandle body,
                                                 jce_vec2 center_local,
                                                 jce_vec2 half_extents,
                                                 float friction,
                                                 float restitution,
                                                 bool sensor);

/* ================================================================== */
/* 2D body state queries                                               */
/* ================================================================== */

void     jce_physics2d_body_get_transform(const JcePhysics2D *world, JceBodyHandle body,
                                          jce_vec2 *out_pos, float *out_angle);
void     jce_physics2d_body_set_transform(JcePhysics2D *world, JceBodyHandle body,
                                          jce_vec2 pos, float angle);

JCE_API jce_vec2 jce_physics2d_body_get_velocity(const JcePhysics2D *world, JceBodyHandle body);
JCE_API void     jce_physics2d_body_set_velocity(JcePhysics2D *world, JceBodyHandle body, jce_vec2 vel);

/* ================================================================== */
/* 2D forces & impulses                                                */
/* ================================================================== */

JCE_API void jce_physics2d_body_apply_force(JcePhysics2D *world, JceBodyHandle body, jce_vec2 force);
JCE_API void jce_physics2d_body_apply_impulse(JcePhysics2D *world, JceBodyHandle body, jce_vec2 impulse);
JCE_API void jce_physics2d_body_apply_torque(JcePhysics2D *world, JceBodyHandle body, float torque);

/* ================================================================== */
/* 2D ray casting                                                      */
/* ================================================================== */

typedef struct {
    bool          hit;
    jce_vec2      point;
    jce_vec2      normal;
    float         fraction;
    JceBodyHandle body;
} JceRaycast2DResult;

JceRaycast2DResult jce_physics2d_raycast(const JcePhysics2D *world,
                                         jce_vec2 origin, jce_vec2 direction,
                                         float max_distance);

/* ================================================================== */
/* Debug                                                               */
/* ================================================================== */

JCE_API uint32_t jce_physics2d_body_count(const JcePhysics2D *world);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS2D_H */
