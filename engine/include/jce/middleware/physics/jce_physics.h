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

    /* Continuous Collision Detection (CCD).  When motion-per-step
     * exceeds `ccd_motion_threshold` the body is swept as a sphere of
     * `ccd_swept_radius` to prevent tunnelling.  Both 0 = CCD off
     * (default).  Recommended for fast-moving small dynamic bodies
     * (bullets, projectiles).  Maps to Bullet's
     * setCcdMotionThreshold/setCcdSweptSphereRadius. */
    float        ccd_motion_threshold;
    float        ccd_swept_radius;
} JceBodyDesc;

JCE_API JceBodyHandle jce_physics_body_create(JcePhysicsWorld *world, const JceBodyDesc *desc);
JCE_API void          jce_physics_body_destroy(JcePhysicsWorld *world, JceBodyHandle body);

/* Create a triangle-mesh STATIC collider.  desc->type is forced to
 * JCE_BODY_STATIC and desc->shape to JCE_SHAPE_MESH internally; the
 * other fields (position/rotation/friction/restitution/collision
 * group/mask/is_trigger) are honoured.
 *
 * Vertices are tightly-packed x,y,z floats (length = vertex_count * 3);
 * indices are uint32 with length = triangle_count * 3.  The caller
 * retains ownership of `vertices` and `indices`; Bullet copies what it
 * needs internally.
 *
 * Dynamic mesh colliders are not supported (Bullet has the same limit);
 * convex hull approximation is the appropriate path for moving meshes
 * — that's a separate API. */
JCE_API JceBodyHandle jce_physics_body_create_mesh(JcePhysicsWorld *world,
                                                    const JceBodyDesc *desc,
                                                    const float    *vertices,
                                                    uint32_t        vertex_count,
                                                    const uint32_t *indices,
                                                    uint32_t        triangle_count);

/* ================================================================== */
/* Body state queries                                                  */
/* ================================================================== */

JCE_API void JCE_CALL jce_physics_body_get_transform(const JcePhysicsWorld *world, JceBodyHandle body,
                                                     jce_vec3 *out_pos, jce_quat *out_rot);
JCE_API void JCE_CALL jce_physics_body_set_transform(JcePhysicsWorld *world, JceBodyHandle body,
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
/* Overlap & shape-cast queries (Unity-style Physics.Overlap*)         */
/* ================================================================== */

/* Collect bodies whose colliders overlap a sphere/box/capsule volume.
 * `collision_mask` filters by collision group (use 0xFFFF for any).
 * Writes up to `max_bodies` body handles to `out_bodies`; returns the
 * actual count.  Identical bodies are de-duplicated.
 *
 * These mirror Unity's Physics.OverlapSphere / Box / Capsule. */
JCE_API uint32_t jce_physics_overlap_sphere(const JcePhysicsWorld *world,
                                            jce_vec3 center, float radius,
                                            uint16_t collision_mask,
                                            JceBodyHandle *out_bodies,
                                            uint32_t max_bodies);

JCE_API uint32_t jce_physics_overlap_box(const JcePhysicsWorld *world,
                                         jce_vec3 center, jce_vec3 half_extents,
                                         jce_quat rotation,
                                         uint16_t collision_mask,
                                         JceBodyHandle *out_bodies,
                                         uint32_t max_bodies);

JCE_API uint32_t jce_physics_overlap_capsule(const JcePhysicsWorld *world,
                                             jce_vec3 center, float radius,
                                             float half_height,
                                             jce_quat rotation,
                                             uint16_t collision_mask,
                                             JceBodyHandle *out_bodies,
                                             uint32_t max_bodies);

/* Boolean shortcuts — true if any collider overlaps the volume. */
JCE_API bool jce_physics_check_sphere(const JcePhysicsWorld *world,
                                      jce_vec3 center, float radius,
                                      uint16_t collision_mask);

JCE_API bool jce_physics_check_box(const JcePhysicsWorld *world,
                                   jce_vec3 center, jce_vec3 half_extents,
                                   jce_quat rotation, uint16_t collision_mask);

JCE_API bool jce_physics_check_capsule(const JcePhysicsWorld *world,
                                       jce_vec3 center, float radius,
                                       float half_height, jce_quat rotation,
                                       uint16_t collision_mask);

/* Sweep a shape from `origin` along `direction` for `max_distance`,
 * returning the closest hit (or hit==false if nothing was struck). */
JCE_API JceRaycastResult jce_physics_sphere_cast(const JcePhysicsWorld *world,
                                                 jce_vec3 origin, float radius,
                                                 jce_vec3 direction,
                                                 float max_distance);

JCE_API JceRaycastResult jce_physics_box_cast(const JcePhysicsWorld *world,
                                              jce_vec3 origin, jce_vec3 half_extents,
                                              jce_quat rotation,
                                              jce_vec3 direction,
                                              float max_distance);

JCE_API JceRaycastResult jce_physics_capsule_cast(const JcePhysicsWorld *world,
                                                  jce_vec3 origin, float radius,
                                                  float half_height,
                                                  jce_quat rotation,
                                                  jce_vec3 direction,
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
JCE_API void JCE_CALL jce_physics_body_set_collision_filter(JcePhysicsWorld *world,
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
JCE_API void JCE_CALL jce_physics_constraint_destroy(JcePhysicsWorld *world,
                                                     JceConstraintHandle con);
JCE_API void JCE_CALL jce_physics_constraint_set_limits(JcePhysicsWorld *world,
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
JCE_API void JCE_CALL jce_physics_character_destroy(JcePhysicsWorld *world,
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

/* ================================================================== */
/* Vehicle controller (raycast wheels)                                 */
/* ================================================================== */

/*
 * GTA-style raycast vehicle.  Internally creates a single dynamic
 * chassis rigid body with a box shape; per-wheel contacts are
 * approximated by downward rays (no real wheel collider).  This is
 * what every open-world driving game uses — full constraint-driven
 * suspension is overkill for street vehicles and 5-10× slower.
 *
 * Coordinate convention: chassis local +X = right, +Y = up,
 *                        +Z = forward.
 *
 * Typical workflow:
 *   1. desc.chassis_half_extents = (0.9, 0.5, 2.2);   // sedan-ish
 *   2. add 4 wheels via JceWheelDesc (FL, FR, RL, RR)
 *   3. Each frame: jce_physics_vehicle_set_input(throttle, brake, steer)
 *   4. Read chassis transform for rendering;
 *      read each wheel's transform for rendering wheels.
 */
typedef struct {
    /* Wheel attachment (chassis local-space). */
    jce_vec3 connection_point;     /* where suspension attaches to chassis */
    jce_vec3 wheel_direction;      /* down vector — typically (0,-1,0) */
    jce_vec3 wheel_axle;           /* spin axis — typically (-1,0,0) */
    float    suspension_rest_len;  /* spring rest length (m) */
    float    wheel_radius;         /* m */
    bool     is_front_wheel;       /* true = steers; false = doesn't */

    /* Tuning (defaults are reasonable street-car values). */
    float    suspension_stiffness;     /* 20.0 */
    float    suspension_damping;       /* 2.3 (relaxation) */
    float    suspension_compression;   /* 4.4 */
    float    friction_slip;            /* 1000.0 (high = sticky tyres) */
    float    roll_influence;           /* 0.1 (lower = less body roll) */
} JceWheelDesc;

typedef struct {
    /* Chassis configuration. */
    jce_vec3 position;
    jce_quat rotation;
    jce_vec3 chassis_half_extents;    /* box half-size around centre */
    float    chassis_mass;            /* kg (≈ 1500 for sedan) */

    /* Drive parameters. */
    float    max_engine_force;        /* N (≈ 2000 for sedan) */
    float    max_brake_force;         /* N (≈ 100 per wheel) */
    float    max_steering_rad;        /* clamp on absolute steer input */

    /* Collision filter for chassis. */
    uint16_t collision_group;
    uint16_t collision_mask;
} JceVehicleDesc;

JCE_API JceVehicleHandle jce_physics_vehicle_create(JcePhysicsWorld *world,
                                                     const JceVehicleDesc *desc);
JCE_API void             jce_physics_vehicle_destroy(JcePhysicsWorld *world,
                                                      JceVehicleHandle veh);

/* Add a wheel to the vehicle.  Returns wheel index (0..N-1) or
 * UINT32_MAX on error.  Must be called before the first step. */
JCE_API uint32_t jce_physics_vehicle_add_wheel(JcePhysicsWorld *world,
                                                JceVehicleHandle veh,
                                                const JceWheelDesc *wheel);

/* Per-frame driver input.  Throttle in [-1,1] (negative = reverse),
 * brake in [0,1], steer in [-1,1] (will be scaled by max_steering_rad). */
JCE_API void jce_physics_vehicle_set_input(JcePhysicsWorld *world,
                                            JceVehicleHandle veh,
                                            float throttle, float brake,
                                            float steer);

/* Chassis transform (centre of mass, world space). */
JCE_API void jce_physics_vehicle_get_chassis_transform(const JcePhysicsWorld *world,
                                                       JceVehicleHandle veh,
                                                       jce_vec3 *out_pos,
                                                       jce_quat *out_rot);

/* Per-wheel world-space transform (rolled + steered).  out_pos is
 * the wheel hub centre. */
JCE_API void jce_physics_vehicle_get_wheel_transform(const JcePhysicsWorld *world,
                                                     JceVehicleHandle veh,
                                                     uint32_t wheel_idx,
                                                     jce_vec3 *out_pos,
                                                     jce_quat *out_rot);

/* Forward speed in m/s (chassis local +Z component of linear velocity).
 * Negative when reversing. */
JCE_API float jce_physics_vehicle_get_speed(const JcePhysicsWorld *world,
                                             JceVehicleHandle veh);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_H */
