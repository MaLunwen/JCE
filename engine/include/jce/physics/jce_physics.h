/*
 * jce_physics.h  3D rigid-body physics (Bullet3 backend).
 *
 * Provides a world with gravity, rigid bodies, and shape colliders.
 * Designed as an optional pluggable subsystem registered via
 * jce_subsystem_register() — the engine core never calls this directly.
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 *
 * Layer: Physics (Layer 3 — optional subsystem, priority 100).
 */

#ifndef JCE_PHYSICS_H
#define JCE_PHYSICS_H

#include <jce/physics/jce_physics_types.h>
#include <jce/core/jce_math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* World lifecycle                                                     */
/* ================================================================== */

typedef struct JcePhysicsWorld JcePhysicsWorld;

typedef struct {
    jce_vec3 gravity;          /* default: (0, -9.81, 0) */
    uint32_t max_bodies;       /* default: 4096 */
    float    fixed_timestep;   /* default: 1/60 */
    int32_t  max_sub_steps;    /* default: 4 */
} JcePhysicsWorldDesc;

JcePhysicsWorld *jce_physics_create(const JcePhysicsWorldDesc *desc);
void             jce_physics_destroy(JcePhysicsWorld *world);

/* ================================================================== */
/* Simulation                                                          */
/* ================================================================== */

/* Step the simulation by dt seconds.  Internally accumulates time
   and runs fixed sub-steps of fixed_timestep length. */
void jce_physics_step(JcePhysicsWorld *world, float dt);

/* ================================================================== */
/* Rigid bodies                                                        */
/* ================================================================== */

typedef struct {
    JceBodyType  type;         /* static / dynamic / kinematic */
    JceShapeType shape;        /* box / sphere / capsule / plane */
    jce_vec3     position;
    jce_quat     rotation;
    jce_vec3     half_extents; /* box: half-size; sphere: (radius,0,0);
                                  capsule: (radius, half_height, 0) */
    float        mass;         /* 0 = static */
    float        friction;     /* default: 0.5 */
    float        restitution;  /* default: 0.0 */
    float        linear_damping;
    float        angular_damping;
} JceBodyDesc;

JceBodyHandle jce_physics_body_create(JcePhysicsWorld *world, const JceBodyDesc *desc);
void          jce_physics_body_destroy(JcePhysicsWorld *world, JceBodyHandle body);

/* ================================================================== */
/* Body state queries                                                  */
/* ================================================================== */

void     jce_physics_body_get_transform(const JcePhysicsWorld *world, JceBodyHandle body,
                                        jce_vec3 *out_pos, jce_quat *out_rot);
void     jce_physics_body_set_transform(JcePhysicsWorld *world, JceBodyHandle body,
                                        jce_vec3 pos, jce_quat rot);

jce_vec3 jce_physics_body_get_velocity(const JcePhysicsWorld *world, JceBodyHandle body);
void     jce_physics_body_set_velocity(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 vel);

jce_vec3 jce_physics_body_get_angular_velocity(const JcePhysicsWorld *world, JceBodyHandle body);
void     jce_physics_body_set_angular_velocity(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 vel);

/* ================================================================== */
/* Forces & impulses                                                   */
/* ================================================================== */

void jce_physics_body_apply_force(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 force);
void jce_physics_body_apply_impulse(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 impulse);
void jce_physics_body_apply_torque(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 torque);

/* ================================================================== */
/* Ray casting                                                         */
/* ================================================================== */

typedef struct {
    bool          hit;
    jce_vec3      point;
    jce_vec3      normal;
    float         distance;
    JceBodyHandle body;
} JceRaycastResult;

JceRaycastResult jce_physics_raycast(const JcePhysicsWorld *world,
                                     jce_vec3 origin, jce_vec3 direction,
                                     float max_distance);

/* ================================================================== */
/* Collision callbacks                                                 */
/* ================================================================== */

void jce_physics_set_contact_begin(JcePhysicsWorld *world, jce_contact_fn fn, void *userdata);
void jce_physics_set_contact_end(JcePhysicsWorld *world, jce_contact_fn fn, void *userdata);

/* ================================================================== */
/* Debug                                                               */
/* ================================================================== */

/* Return current body count (active + sleeping). */
uint32_t jce_physics_body_count(const JcePhysicsWorld *world);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PHYSICS_H */
