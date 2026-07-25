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

    /* Solver / sleeping tunables.  All optional — a zero / negative value
       leaves Bullet's stock default in place, reproducing prior behavior. */
    int32_t  solver_iterations;        /* 0 = Bullet default (10) */
    int32_t  split_impulse;            /* -1 = leave default; 0 = off; 1 = on */
    float    deactivation_time;        /* seconds; 0 = leave default (2.0).
                                          GLOBAL: sets Bullet's gDeactivationTime
                                          for all bodies in the process. */
    float    linear_sleep_threshold;   /* 0 = leave per-body default (0.8) */
    float    angular_sleep_threshold;  /* 0 = leave per-body default (1.0) */

    /* Opt-in multithreaded Bullet solver.  Only honoured when the engine
       was compiled with JCE_PHYSICS_MT (which in turn needs Bullet built
       with bt2_thread_locks=True / BT_THREADSAFE).  Ignored (single-thread
       path used, with a one-time warning) on a stock single-threaded build.
       Left OFF by default: the MT solver's island/constraint ordering is
       non-deterministic and conflicts with the fixed-step determinism. */
    bool     multithreaded;            /* default: false (single-threaded) */
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
    uint32_t     collision_group; /* default: JCE_COLLISION_DEFAULT_GROUP */
    uint32_t     collision_mask;  /* default: JCE_COLLISION_ALL_MASK */
    bool         is_trigger;      /* trigger bodies: no contact response */
} JceBodyDesc;

JCE_API JceBodyHandle jce_physics_body_create(JcePhysicsWorld *world, const JceBodyDesc *desc);
JCE_API void          jce_physics_body_destroy(JcePhysicsWorld *world, JceBodyHandle body);

/* ================================================================== */
/* Compound / mesh bodies (per-object colliders)                       */
/* ================================================================== */

/*
 * A single child collider inside a compound body.  Each child carries
 * its own shape, a transform LOCAL to the owning body, and (for hull /
 * triangle-mesh shapes) borrowed geometry that is copied into Bullet at
 * create time — the caller may free `vertices` / `indices` afterwards.
 *
 * The whole point of the compound is that a model with N separated
 * objects produces N children rather than one fat box/hull that fills
 * the empty space between them.
 */
typedef struct {
    JceShapeType shape;        /* BOX/SPHERE/CAPSULE/CONVEX_HULL/TRIANGLE_MESH */
    jce_vec3     position;     /* child origin, relative to the body */
    jce_quat     rotation;     /* child rotation, relative to the body */

    /* Primitive parameters (box: half-size; sphere: (radius,0,0);
       capsule: (radius, half_height, 0)).  Ignored for hull / mesh. */
    jce_vec3     half_extents;

    /* Geometry for CONVEX_HULL (point cloud) and TRIANGLE_MESH.
       `vertices` is xyz-triplets, `vertex_count` points.
       `indices` (TRIANGLE_MESH only) is 3 per triangle. */
    const float    *vertices;
    uint32_t        vertex_count;
    const uint32_t *indices;
    uint32_t        index_count;
} JceColliderChild;

typedef struct {
    JceBodyType  type;            /* static / dynamic / kinematic */
    jce_vec3     position;        /* body origin */
    jce_quat     rotation;        /* body rotation */
    float        mass;            /* 0 = static */
    float        friction;        /* default 0.5 when <= 0 */
    float        restitution;
    float        linear_damping;
    float        angular_damping;
    uint32_t     collision_group; /* default group when 0 */
    uint32_t     collision_mask;  /* default all when 0 */
    bool         is_trigger;

    const JceColliderChild *children;
    uint32_t                child_count;
} JceCompoundBodyDesc;

/* Create a body whose collision shape is a compound of `child_count`
   children (or, when there is a single child at identity transform, the
   child shape directly).  TRIANGLE_MESH children require a non-dynamic
   body.  Returns JCE_BODY_INVALID on failure. */
JCE_API JceBodyHandle jce_physics_body_create_compound(JcePhysicsWorld           *world,
                                                       const JceCompoundBodyDesc *desc);

/* ================================================================== */
/* Body state queries                                                  */
/* ================================================================== */

JCE_API void JCE_CALL jce_physics_body_get_transform(const JcePhysicsWorld *world, JceBodyHandle body,
                                                     jce_vec3 *out_pos, jce_quat *out_rot);
JCE_API void JCE_CALL jce_physics_body_set_transform(JcePhysicsWorld *world, JceBodyHandle body,
                                                     jce_vec3 pos, jce_quat rot);

/* True for a movable (DYNAMIC) rigid body; false for static/kinematic/invalid. */
JCE_API bool     jce_physics_body_is_dynamic(const JcePhysicsWorld *world, JceBodyHandle body);
JCE_API jce_vec3 jce_physics_body_get_velocity(const JcePhysicsWorld *world, JceBodyHandle body);
JCE_API void     jce_physics_body_set_velocity(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 vel);
/* Sleep (active=false: DISABLE_SIMULATION + zero velocity) or wake (active=true)
 * a body — used by the runtime's sim-LOD physics gating to stop the solver from
 * integrating far-tier actors.  Forced: a slept body stays asleep until woken. */
JCE_API void     jce_physics_body_set_active(JcePhysicsWorld *world, JceBodyHandle body, bool active);

JCE_API jce_vec3 jce_physics_body_get_angular_velocity(const JcePhysicsWorld *world, JceBodyHandle body);
JCE_API void     jce_physics_body_set_angular_velocity(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 vel);

/* ================================================================== */
/* Forces & impulses                                                   */
/* ================================================================== */

JCE_API void jce_physics_body_apply_force(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 force);
JCE_API void jce_physics_body_apply_impulse(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 impulse);
JCE_API void jce_physics_body_apply_torque(JcePhysicsWorld *world, JceBodyHandle body, jce_vec3 torque);

/* Apply a force / impulse at a world-space point (generates torque about
 * the centre of mass).  Mirrors Unity's ForceMode.Force/Impulse with an
 * application point — needed for explosions, thrusters, hit reactions. */
JCE_API void JCE_CALL jce_physics_body_apply_force_at_point(JcePhysicsWorld *world,
                                                            JceBodyHandle body,
                                                            jce_vec3 force,
                                                            jce_vec3 world_point);
JCE_API void JCE_CALL jce_physics_body_apply_impulse_at_point(JcePhysicsWorld *world,
                                                              JceBodyHandle body,
                                                              jce_vec3 impulse,
                                                              jce_vec3 world_point);

/* ================================================================== */
/* Per-body gravity & mass                                             */
/* ================================================================== */

/* Scale world gravity for a single body.  factor 1 = normal gravity,
 * 0 = unaffected by gravity (floating platform / pickup), negative =
 * inverted.  Unity's `useGravity=false` maps to factor 0; Godot's
 * `gravity_scale` maps directly.  Must be applied after the body is in
 * the world (body creation resets per-body gravity to the world value). */
JCE_API void JCE_CALL jce_physics_body_set_gravity_factor(JcePhysicsWorld *world,
                                                          JceBodyHandle body,
                                                          float factor);

/* Per-axis angular factor.  (0,0,0) locks all rotation (Unity FreezeRotation /
 * Godot lock_rotation) so a dynamic body never tips/rolls yet still collides
 * linearly; (1,1,1) = free.  Apply after the body is in the world. */
JCE_API void JCE_CALL jce_physics_body_set_angular_factor(JcePhysicsWorld *world,
                                                          JceBodyHandle body,
                                                          jce_vec3 factor);

/* Change a dynamic body's mass at runtime; recomputes the local inertia
 * tensor from the current collision shape.  mass <= 0 makes the body
 * effectively static (zero inverse mass). */
JCE_API void JCE_CALL jce_physics_body_set_mass(JcePhysicsWorld *world,
                                                JceBodyHandle body,
                                                float mass);

/* Set the collider's local scale at runtime (Unity-style — the collider
 * follows the entity's Transform scale).  Recomputes the inertia tensor for
 * dynamic bodies and refreshes the broadphase AABB.  `scale` is relative to
 * the shape's authored size. */
JCE_API void JCE_CALL jce_physics_body_set_scale(JcePhysicsWorld *world,
                                                 JceBodyHandle body,
                                                 jce_vec3 scale);

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

JCE_API JceRaycastResult jce_physics_raycast(const JcePhysicsWorld *world,
                                     jce_vec3 origin, jce_vec3 direction,
                                     float max_distance);

/* ================================================================== */
/* Filtered spatial queries                                            */
/* ================================================================== */

/* Query filter.  layer_mask: bit i selects physics layer i (0xFFFFFFFF =
 * every layer).  hit_triggers: when false (default) sensor/trigger bodies
 * are skipped.  Use jce_query_filter_default() for "everything, no triggers". */
typedef struct {
    uint32_t layer_mask;
    bool     hit_triggers;
} JceQueryFilter;

static inline JceQueryFilter jce_query_filter_default(void)
{
    JceQueryFilter f;
    f.layer_mask   = 0xFFFFFFFFu;
    f.hit_triggers = false;
    return f;
}

/* Closest-hit raycast honoring the layer mask + trigger skip. */
JCE_API JceRaycastResult JCE_CALL jce_physics_raycast_filtered(
        const JcePhysicsWorld *world, jce_vec3 origin, jce_vec3 direction,
        float max_distance, JceQueryFilter filter);

/* Multi-hit raycast.  Writes up to max_hits results (sorted near→far) into
 * out_hits and returns the number written. */
JCE_API uint32_t JCE_CALL jce_physics_raycast_all(
        const JcePhysicsWorld *world, jce_vec3 origin, jce_vec3 direction,
        float max_distance, JceQueryFilter filter,
        JceRaycastResult *out_hits, uint32_t max_hits);

/* Overlap tests: write up to max_bodies overlapping body handles into
 * out_bodies and return the count. */
JCE_API uint32_t JCE_CALL jce_physics_overlap_sphere(
        const JcePhysicsWorld *world, jce_vec3 center, float radius,
        JceQueryFilter filter, JceBodyHandle *out_bodies, uint32_t max_bodies);
JCE_API uint32_t JCE_CALL jce_physics_overlap_box(
        const JcePhysicsWorld *world, jce_vec3 center, jce_vec3 half_extents,
        jce_quat rotation, JceQueryFilter filter,
        JceBodyHandle *out_bodies, uint32_t max_bodies);

/* Sweep a sphere of `radius` from origin along direction; returns the first
 * hit (a thick raycast — for projectiles / character movement). */
JCE_API JceRaycastResult JCE_CALL jce_physics_sweep_sphere(
        const JcePhysicsWorld *world, jce_vec3 origin, float radius,
        jce_vec3 direction, float max_distance, JceQueryFilter filter);

/* ================================================================== */
/* Continuous Collision Detection (CCD)  (P3-C.3)                      */
/* ================================================================== */

/*
 * Per-body collision-detection mode.  Unity parity.
 *
 * DISCRETE                — Bullet default.  Fastest, but fast-moving
 *                           bodies may tunnel through thin colliders.
 * CONTINUOUS              — Swept CCD against static / kinematic
 *                           geometry.  ~2-5× more expensive than
 *                           discrete; use only for small fast objects
 *                           (bullets, thrown items).
 * CONTINUOUS_DYNAMIC      — Same as CONTINUOUS in this Bullet build.
 *                           Bullet's swept CCD already checks against
 *                           dynamic bodies whenever both sides have a
 *                           non-zero motion threshold; kept as a
 *                           distinct enum for Unity parity / future
 *                           backends.
 * CONTINUOUS_SPECULATIVE  — Speculative contacts are a world-wide
 *                           solver toggle in Bullet, not a per-body
 *                           flag.  Aliased to CONTINUOUS for now; the
 *                           inspector still records the user's choice
 *                           so it round-trips through save/load.
 */
typedef enum JceCcdMode {
    JCE_CCD_DISCRETE              = 0,
    JCE_CCD_CONTINUOUS            = 1,
    JCE_CCD_CONTINUOUS_DYNAMIC    = 2,
    JCE_CCD_CONTINUOUS_SPECULATIVE = 3
} JceCcdMode;

/* Sensible Bullet defaults applied by set_ccd_mode when mode != DISCRETE.
 * Threshold = distance per frame above which CCD kicks in. */
#define JCE_CCD_DEFAULT_MOTION_THRESHOLD 0.01f

/* Switch the body's CCD mode.  When enabling CCD, motion threshold is
 * reset to the default and the swept-sphere radius is auto-computed
 * from the body's collision shape AABB (≈ 0.5 × min half-extent).
 * Use the explicit setters below to override after enabling. */
JCE_API void       JCE_CALL jce_physics_body_set_ccd_mode(JcePhysicsWorld *world,
                                                          JceBodyHandle body,
                                                          JceCcdMode mode);
JCE_API JceCcdMode JCE_CALL jce_physics_body_get_ccd_mode(const JcePhysicsWorld *world,
                                                          JceBodyHandle body);

/* Motion threshold (metres / frame).  0 disables CCD on Bullet's side. */
JCE_API void  JCE_CALL jce_physics_body_set_ccd_motion_threshold(JcePhysicsWorld *world,
                                                                 JceBodyHandle body,
                                                                 float threshold);
JCE_API float JCE_CALL jce_physics_body_get_ccd_motion_threshold(const JcePhysicsWorld *world,
                                                                 JceBodyHandle body);

/* Embedded swept-sphere radius used by Bullet's CCD solver.  Should be
 * a little smaller than the shape's smallest half-extent. */
JCE_API void  JCE_CALL jce_physics_body_set_ccd_swept_sphere_radius(JcePhysicsWorld *world,
                                                                    JceBodyHandle body,
                                                                    float radius);
JCE_API float JCE_CALL jce_physics_body_get_ccd_swept_sphere_radius(const JcePhysicsWorld *world,
                                                                    JceBodyHandle body);

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
                                                            uint32_t group, uint32_t mask);

/* ================================================================== */
/* Material                                                            */
/* ================================================================== */

/* Apply a JcePhysicsMaterial to a live body.  Sets the body's friction
 * to material->dynamic_friction and restitution to material->restitution.
 * Bullet's per-contact combine (multiply for friction, max for
 * restitution) runs on these per-body values.  For JCE-level combine
 * semantics, use jce_physics_material_combine() at material-set time. */
struct JcePhysicsMaterial;
JCE_API void JCE_CALL jce_physics_body_set_material(JcePhysicsWorld *world,
                                                    JceBodyHandle body,
                                                    const struct JcePhysicsMaterial *material);

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

JCE_API JceConstraintHandle jce_physics_constraint_create(JcePhysicsWorld *world,
                                                   const JceConstraintDesc *desc);
JCE_API void JCE_CALL jce_physics_constraint_destroy(JcePhysicsWorld *world,
                                                     JceConstraintHandle con);
JCE_API void JCE_CALL jce_physics_constraint_set_limits(JcePhysicsWorld *world,
                                                        JceConstraintHandle con,
                                                        float lower, float upper);

/* ── Configurable joint (Unity-style per-axis 6DOF + break) ────────────
 *
 * A generic 6-DOF joint with INDEPENDENT motion authoring per linear and
 * angular axis (Locked / Limited / Free), on top of which the runtime layers
 * a break-force monitor.  Distinct from JceConstraintDesc's single-limit-pair
 * 6DOF: here every axis carries its own constraint state, which Unity's
 * ConfigurableJoint exposes and the simple typed constraints do not.
 *
 * Built on a btGeneric6DofConstraint and stored in the SAME constraint
 * registry as jce_physics_constraint_create, so the returned handle is
 * destroyed via jce_physics_constraint_destroy and queried via
 * jce_physics_constraint_applied_impulse. */
typedef struct {
    JceBodyHandle body_a;
    JceBodyHandle body_b;          /* JCE_BODY_INVALID = world anchor */
    jce_vec3      anchor_a;        /* local anchor on body A */
    jce_vec3      anchor_b;        /* local anchor on body B (or world) */
    int           lin_motion[3];   /* X,Y,Z: 0=locked 1=limited 2=free */
    int           ang_motion[3];   /* X,Y,Z: 0=locked 1=limited 2=free */
    float         linear_limit;        /* symmetric ±metres on limited linear axes */
    float         angular_limit_deg[3];/* symmetric ±deg per limited angular axis */
    bool          disable_collision;   /* disable collision between A and B */
} JceConfigurableJointDesc;

JCE_API JceConstraintHandle JCE_CALL jce_physics_configurable_joint_create(
        JcePhysicsWorld *world, const JceConfigurableJointDesc *desc);

/* Last-step applied-impulse magnitude (N·s) of a live constraint, or 0 for an
 * invalid handle.  Used by the runtime break-force monitor: an applied IMPULSE
 * is compared against (break_FORCE * fixed_dt) since impulse = force * dt. */
JCE_API float JCE_CALL jce_physics_constraint_applied_impulse(
        const JcePhysicsWorld *world, JceConstraintHandle con);

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
    /* Movement feel (0 = engine default). `accel` is the horizontal
     * acceleration toward the commanded velocity (m/s^2); `air_control`
     * scales it while airborne (0..1). */
    float    accel;
    float    air_control;
} JceCharacterDesc;

JCE_API JceCharacterHandle jce_physics_character_create(JcePhysicsWorld *world,
                                                 const JceCharacterDesc *desc);
JCE_API void JCE_CALL jce_physics_character_destroy(JcePhysicsWorld *world,
                                                    JceCharacterHandle ch);
JCE_API void jce_physics_character_move(JcePhysicsWorld *world,
                                 JceCharacterHandle ch,
                                 jce_vec3 walk_dir, float dt);
/* Returns true when the jump fired (false while already mid-jump). */
JCE_API bool jce_physics_character_jump(JcePhysicsWorld *world,
                                 JceCharacterHandle ch);
JCE_API void jce_physics_character_get_position(const JcePhysicsWorld *world,
                                         JceCharacterHandle ch,
                                         jce_vec3 *out_pos);
/* Teleport the character to the given capsule-CENTER position. */
JCE_API void jce_physics_character_set_position(JcePhysicsWorld *world,
                                         JceCharacterHandle ch,
                                         jce_vec3 pos);
JCE_API bool jce_physics_character_is_grounded(const JcePhysicsWorld *world,
                                        JceCharacterHandle ch);
/* Live linear velocity of the capsule body (m/s). */
JCE_API void jce_physics_character_get_velocity(const JcePhysicsWorld *world,
                                         JceCharacterHandle ch,
                                         jce_vec3 *out_vel);
/* Early jump release: scales any remaining upward velocity by `factor`
 * (0..1) so short taps yield short hops.  No-op when not ascending. */
JCE_API void jce_physics_character_cut_jump(JcePhysicsWorld *world,
                                     JceCharacterHandle ch,
                                     float factor);

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
    uint32_t collision_group;
    uint32_t collision_mask;
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
