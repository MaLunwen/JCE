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


#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

JCE_EXTERN_C_BEGIN

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

JCE_API JcePhysicsWorld *jce_physics_create(const JcePhysicsWorldDesc *desc);
JCE_API void             jce_physics_destroy(JcePhysicsWorld *world);

/* ================================================================== */
/* Simulation                                                          */
/* ================================================================== */

/* Step the simulation by dt seconds.  Internally accumulates time
   and runs fixed sub-steps of fixed_timestep length. */
JCE_API void jce_physics_step(JcePhysicsWorld *world, float dt);

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
    uint16_t     collision_group; /* default: JCE_COLLISION_DEFAULT_GROUP */
    uint16_t     collision_mask;  /* default: JCE_COLLISION_ALL_MASK */
    bool         is_trigger;      /* trigger bodies: no contact response */
} JceBodyDesc;

JCE_API JceBodyHandle jce_physics_body_create(JcePhysicsWorld *world, const JceBodyDesc *desc);
JCE_API void          jce_physics_body_destroy(JcePhysicsWorld *world, JceBodyHandle body);

/* ================================================================== */
/* Body state queries                                                  */
/* ================================================================== */

void     jce_physics_body_get_transform(const JcePhysicsWorld *world, JceBodyHandle body,
                                        jce_vec3 *out_pos, jce_quat *out_rot);
void     jce_physics_body_set_transform(JcePhysicsWorld *world, JceBodyHandle body,
                                        jce_vec3 pos, jce_quat rot);

JCE_API jce_vec3 jce_physics_body_get_velocity(const JcePhysicsWorld *world, JceBodyHandle body);
JCE_API void     jce_physics_body_set_velocity(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 vel);

JCE_API jce_vec3 jce_physics_body_get_angular_velocity(const JcePhysicsWorld *world, JceBodyHandle body);
JCE_API void     jce_physics_body_set_angular_velocity(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 vel);

/* ================================================================== */
/* Forces & impulses                                                   */
/* ================================================================== */

JCE_API void jce_physics_body_apply_force(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 force);
JCE_API void jce_physics_body_apply_impulse(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 impulse);
JCE_API void jce_physics_body_apply_torque(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 torque);

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

JCE_API void jce_physics_set_contact_begin(JcePhysicsWorld *world, jce_contact_fn fn, void *userdata);
JCE_API void jce_physics_set_contact_end(JcePhysicsWorld *world, jce_contact_fn fn, void *userdata);

/* ================================================================== */
/* Debug                                                               */
/* ================================================================== */

/* Return current body count (active + sleeping). */
JCE_API uint32_t jce_physics_body_count(const JcePhysicsWorld *world);

/* ================================================================== */
/* Collision filters                                                   */
/* ================================================================== */

/* Change collision group/mask on an existing body. */
void jce_physics_body_set_collision_filter(JcePhysicsWorld *world,
                                           JceBodyHandle body,
                                           uint16_t group, uint16_t mask);

/* ================================================================== */
/* Constraints                                                         */
/* ================================================================== */

typedef struct {
    JceConstraintType type;
    JceBodyHandle     body_a;
    JceBodyHandle     body_b;       /* JCE_BODY_INVALID = world anchor */
    jce_vec3          pivot_a;      /* local pivot on body A */
    jce_vec3          pivot_b;      /* local pivot on body B */
    jce_vec3          axis;         /* hinge/slider axis (local to A) */
    float             lower_limit;
    float             upper_limit;
    bool              disable_collision; /* disable collision between A and B */
} JceConstraintDesc;

JceConstraintHandle jce_physics_constraint_create(JcePhysicsWorld *world,
                                                   const JceConstraintDesc *desc);
void jce_physics_constraint_destroy(JcePhysicsWorld *world,
                                     JceConstraintHandle con);
void jce_physics_constraint_set_limits(JcePhysicsWorld *world,
                                        JceConstraintHandle con,
                                        float lower, float upper);

/* ================================================================== */
/* Character controller                                                */
/* ================================================================== */

typedef struct {
    jce_vec3 position;
    float    radius;           /* capsule radius */
    float    height;           /* capsule total height */
    float    step_height;      /* max step height */
    float    max_slope_deg;    /* max walkable slope (degrees) */
    float    gravity;          /* character gravity (positive = downward) */
    float    jump_speed;       /* initial jump velocity */
} JceCharacterDesc;

JceCharacterHandle jce_physics_character_create(JcePhysicsWorld *world,
                                                 const JceCharacterDesc *desc);
void jce_physics_character_destroy(JcePhysicsWorld *world,
                                    JceCharacterHandle ch);
void jce_physics_character_move(JcePhysicsWorld *world,
                                 JceCharacterHandle ch,
                                 jce_vec3 walk_dir, float dt);
void jce_physics_character_jump(JcePhysicsWorld *world,
                                 JceCharacterHandle ch);
void jce_physics_character_get_position(const JcePhysicsWorld *world,
                                         JceCharacterHandle ch,
                                         jce_vec3 *out_pos);
bool jce_physics_character_is_grounded(const JcePhysicsWorld *world,
                                        JceCharacterHandle ch);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_H */
