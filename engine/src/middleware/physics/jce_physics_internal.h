/*
 * jce_physics_internal.h  Internal bridge between C99 front-end and
 *                         C++ Bullet3 back-end.
 *
 * All functions use the jce_bullet_* prefix.  They are implemented in
 * jce_physics_bullet.cpp and called exclusively from jce_physics.c.
 */

#ifndef JCE_PHYSICS_INTERNAL_H
#define JCE_PHYSICS_INTERNAL_H

#include <jce/middleware/physics/jce_physics_material.h>
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Opaque Bullet world handle                                         */
/* ================================================================== */

typedef struct JceBulletWorld JceBulletWorld;

/* ================================================================== */
/* World lifecycle                                                     */
/* ================================================================== */

/* Solver / sleeping tunables.  Sentinel values keep Bullet's stock
   defaults: solver_iterations<=0, split_impulse<0, deactivation_time<=0,
   linear/angular_sleep_threshold<=0 all mean "leave default".
   `multithreaded` requests the parallel solver/dispatcher path; it is only
   honoured when the back-end was compiled with JCE_PHYSICS_MT (otherwise it
   logs once and falls back to the single-threaded world). */
JceBulletWorld *jce_bullet_create(jce_vec3 gravity, uint32_t max_bodies,
                                  int solver_iterations, int split_impulse,
                                  float deactivation_time,
                                  float linear_sleep_threshold,
                                  float angular_sleep_threshold,
                                  bool multithreaded);
void            jce_bullet_destroy(JceBulletWorld *bw);

/* ================================================================== */
/* Simulation step                                                     */
/* ================================================================== */

void jce_bullet_step(JceBulletWorld *bw, float dt, float fixed_dt,
                     int max_sub_steps);

/* ================================================================== */
/* Rigid-body management                                               */
/* ================================================================== */

uint32_t jce_bullet_body_create(JceBulletWorld *bw,
                                /* JceBodyType  */ uint8_t type,
                                /* JceShapeType */ uint8_t shape,
                                jce_vec3 pos, jce_quat rot,
                                jce_vec3 half_ext, float mass,
                                float friction, float restitution,
                                float lin_damp, float ang_damp,
                                uint32_t col_group, uint32_t col_mask,
                                bool is_trigger,
                                /* JceCapsuleAxis */ uint8_t capsule_axis);

void jce_bullet_body_destroy(JceBulletWorld *bw, uint32_t idx);

/* ------------------------------------------------------------------ */
/* Generation-counter handles (D-gen-handles)                          */
/* ------------------------------------------------------------------ */

/* Current generation counter for pool slot `idx`.  Bumped every time the
 * slot is destroyed, so a handle minted from an older generation can be
 * detected as stale.  Returns 0 for an out-of-range slot (the value a
 * fresh, never-recycled slot also reports). */
uint32_t jce_bullet_body_generation(JceBulletWorld *bw, uint32_t idx);

/* Validate that slot `idx` is in range, alive, and still on generation
 * `gen`.  This is the stale-handle guard: a handle whose generation no
 * longer matches the slot's (because the slot was freed and reused) is
 * rejected.  gen 0 against a never-recycled slot always matches, so bare
 * pre-existing handles stay valid. */
bool jce_bullet_body_alive_gen(JceBulletWorld *bw, uint32_t idx, uint32_t gen);

/* ------------------------------------------------------------------ */
/* Compound / mesh bodies                                              */
/* ------------------------------------------------------------------ */

/* Plain-C mirror of JceColliderChild so this header keeps its
 * zero-dependency stance on the physics public headers. */
typedef struct {
    uint8_t         shape;        /* JceShapeType */
    jce_vec3        position;
    jce_quat        rotation;
    jce_vec3        half_extents;
    const float    *vertices;     /* xyz triplets */
    uint32_t        vertex_count;
    const uint32_t *indices;
    uint32_t        index_count;
    uint8_t         capsule_axis; /* JceCapsuleAxis; 0 = Y */
} JceBulletColliderChild;

uint32_t jce_bullet_body_create_compound(JceBulletWorld *bw,
                                         uint8_t type,
                                         jce_vec3 pos, jce_quat rot,
                                         float mass, float friction,
                                         float restitution,
                                         float lin_damp, float ang_damp,
                                         uint32_t col_group, uint32_t col_mask,
                                         bool is_trigger,
                                         const JceBulletColliderChild *children,
                                         uint32_t child_count);

/* Static heightfield.  `pos` is the field's MIN corner in world space; the
 * bridge applies Bullet's centre-of-AABB origin compensation internally so
 * callers never see it.  Samples are copied into shape-owned storage before
 * return.  `diagonal` is JceHeightfieldDiagonal. */
uint32_t jce_bullet_body_create_heightfield(JceBulletWorld *bw,
                                            jce_vec3 pos, jce_quat rot,
                                            const float *heights,
                                            uint32_t samples_x,
                                            uint32_t samples_z,
                                            float cell_size_x,
                                            float cell_size_z,
                                            float min_height,
                                            float max_height,
                                            uint8_t diagonal,
                                            float friction, float restitution,
                                            uint32_t col_group,
                                            uint32_t col_mask,
                                            bool is_trigger,
                                            bool smooth_internal_edges);

/* ================================================================== */
/* Transform                                                           */
/* ================================================================== */

void jce_bullet_body_get_transform(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 *pos, jce_quat *rot);
void jce_bullet_body_set_transform(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 pos, jce_quat rot);

/* ================================================================== */
/* Velocity                                                            */
/* ================================================================== */

void jce_bullet_body_get_velocity(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 *vel);
void jce_bullet_body_set_velocity(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 vel);
void jce_bullet_body_set_active(JceBulletWorld *bw, uint32_t idx, bool active);
void jce_bullet_body_get_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 *vel);
void jce_bullet_body_set_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 vel);

/* ================================================================== */
/* Forces & impulses                                                   */
/* ================================================================== */

bool jce_bullet_body_is_dynamic(JceBulletWorld *bw, uint32_t idx);
void jce_bullet_body_apply_force(JceBulletWorld *bw, uint32_t idx,
                                 jce_vec3 force);
void jce_bullet_body_apply_impulse(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 impulse);
void jce_bullet_body_apply_torque(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 torque);

void jce_bullet_body_apply_force_at_point(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 force, jce_vec3 world_point);
void jce_bullet_body_apply_impulse_at_point(JceBulletWorld *bw, uint32_t idx,
                                            jce_vec3 impulse, jce_vec3 world_point);

/* Per-body gravity (scales the world gravity vector) and runtime mass. */
void jce_bullet_body_set_gravity_factor(JceBulletWorld *bw, uint32_t idx,
                                        float factor);
void jce_bullet_body_set_angular_factor(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 factor);
void jce_bullet_body_set_mass(JceBulletWorld *bw, uint32_t idx, float mass);

/* Runtime collider scale: sets the shape's local scaling, recomputes inertia
 * for dynamic bodies, and refreshes the broadphase AABB. */
void jce_bullet_body_set_scale(JceBulletWorld *bw, uint32_t idx, jce_vec3 scale);

/* ================================================================== */
/* Ray casting                                                         */
/* ================================================================== */

typedef struct {
    bool     hit;
    jce_vec3 point;
    jce_vec3 normal;
    float    distance;
    uint32_t body_idx;
} JceBulletRayResult;

JceBulletRayResult jce_bullet_raycast(JceBulletWorld *bw, jce_vec3 origin,
                                      jce_vec3 dir, float max_dist);

/* ------------------------------------------------------------------ */
/* Filtered spatial queries (layer_mask = bit i selects layer i;      */
/* hit_triggers includes CF_NO_CONTACT_RESPONSE bodies).              */
/* ------------------------------------------------------------------ */

JceBulletRayResult jce_bullet_raycast_filtered(JceBulletWorld *bw,
                                               jce_vec3 origin, jce_vec3 dir,
                                               float max_dist,
                                               uint32_t layer_mask,
                                               bool hit_triggers);

/* Multi-hit ray: fills out[] (sorted near→far), returns count written. */
uint32_t jce_bullet_raycast_all(JceBulletWorld *bw,
                                jce_vec3 origin, jce_vec3 dir, float max_dist,
                                uint32_t layer_mask, bool hit_triggers,
                                JceBulletRayResult *out, uint32_t max_hits);

/* Overlap tests: fill out_idx[] with overlapping body indices, return count. */
uint32_t jce_bullet_overlap_sphere(JceBulletWorld *bw, jce_vec3 center,
                                   float radius, uint32_t layer_mask,
                                   bool hit_triggers,
                                   uint32_t *out_idx, uint32_t max);
uint32_t jce_bullet_overlap_box(JceBulletWorld *bw, jce_vec3 center,
                                jce_vec3 half_ext, jce_quat rot,
                                uint32_t layer_mask, bool hit_triggers,
                                uint32_t *out_idx, uint32_t max);

/* Sphere sweep: closest hit along dir (a thick ray). */
JceBulletRayResult jce_bullet_sweep_sphere(JceBulletWorld *bw, jce_vec3 origin,
                                           float radius, jce_vec3 dir,
                                           float max_dist, uint32_t layer_mask,
                                           bool hit_triggers);

/* ================================================================== */
/* Contact callbacks                                                   */
/* ================================================================== */

/* is_trigger is CF_NO_CONTACT_RESPONSE on either object.  It is passed
 * rather than looked up because the dispatch site already holds both
 * btCollisionObjects; the C trampoline that builds JceContactEvent holds
 * only indices, and it silently shipped is_trigger=false for every contact
 * because `memset(&ev, 0, ...)` makes "not a trigger" the default and
 * "not a trigger" is a perfectly legal value. */
typedef void (*jce_bullet_contact_fn)(uint32_t body_a, uint32_t body_b,
                                      const float normal[3],
                                      const float point[3],
                                      float depth,
                                      float applied_impulse,
                                      bool is_trigger, void *ud);

void jce_bullet_set_contact_begin(JceBulletWorld *bw,
                                  jce_bullet_contact_fn fn, void *ud);
void jce_bullet_set_contact_end(JceBulletWorld *bw,
                                jce_bullet_contact_fn fn, void *ud);

/* ================================================================== */
/* Utility                                                             */
/* ================================================================== */

uint32_t jce_bullet_body_count(JceBulletWorld *bw);

/* Returns true if the body has the trigger flag (CF_NO_CONTACT_RESPONSE)
 * set.  Used by the C layer when populating JceContactEvent.is_trigger. */
bool jce_bullet_body_is_trigger(JceBulletWorld *bw, uint32_t idx);

/* ================================================================== */
/* Manifold enumeration (P3-C.5 — trigger / contact events)            */
/* ================================================================== */

/* Per-pair callback.  `is_trigger` is true if either body is a sensor.
 * Coordinates are world-space.  depth is positive penetration. */
typedef void (*jce_bullet_pair_fn)(uint32_t body_a, uint32_t body_b,
                                   const float normal[3],
                                   const float point[3],
                                   float depth, bool is_trigger,
                                   void *ud);

/* Walk every persistent manifold with >=1 active contact point and
 * invoke `fn` once per pair (using the first / deepest contact).
 * Called from the C layer right after jce_bullet_step(). */
void jce_bullet_enumerate_pairs(JceBulletWorld *bw,
                                jce_bullet_pair_fn fn, void *ud);

/* ================================================================== */
/* Debug draw bridge (P3-C.5)                                          */
/* ================================================================== */

/* Line-sink signature passed through to the debug drawer.  Coordinates
 * are world-space; abgr is 0xAABBGGRR. */
typedef void (*jce_bullet_line_fn)(float fx, float fy, float fz,
                                   float tx, float ty, float tz,
                                   uint32_t abgr, void *ud);

/* Set the debug-draw mode (mirrors btIDebugDraw mode bits — same
 * values exposed by JcePhysicsDebugFlag).  Installs / removes the
 * btIDebugDraw subclass on first non-zero / zero transition. */
void jce_bullet_debug_set_mode(JceBulletWorld *bw, uint32_t flags);

/* Drive Bullet's debug-draw pass and forward every line to `fn`.
 * No-op when the installed mode is 0 or `fn` is NULL. */
void jce_bullet_debug_draw(JceBulletWorld *bw,
                           jce_bullet_line_fn fn, void *ud);

/* Internal: release the debug-drawer slot when the world is torn
 * down.  Called by jce_bullet_destroy(). */
void jce_bullet_debug_world_destroyed_(JceBulletWorld *bw);

/* ================================================================== */
/* Collision filter                                                    */
/* ================================================================== */

void jce_bullet_body_set_collision_filter(JceBulletWorld *bw, uint32_t idx,
                                          uint32_t group, uint32_t mask);

/* Set friction + restitution on an existing body, and the combine modes that
 * decide how they meet another body's at a contact.
 *
 * Bullet stores friction and restitution per body and combines them with a
 * FIXED rule (multiply, and max).  The modes cannot be pre-baked into the
 * per-body values because a combine is a function of the PAIR, so they are
 * carried on the body and applied in the contact-added callback.  Calling
 * this marks the body CF_CUSTOM_MATERIAL_CALLBACK; a body that never gets a
 * material keeps Bullet's own combine untouched. */
void jce_bullet_body_set_material(JceBulletWorld *bw, uint32_t idx,
                                  float friction, float restitution,
                                  JcePhysicsCombine friction_combine,
                                  JcePhysicsCombine restitution_combine);

/* ================================================================== */
/* Continuous Collision Detection (CCD)  (P3-C.3)                      */
/* ================================================================== */

/* Returns 0.0f if idx invalid.  swept_sphere_radius=0 means "auto":
 * derive it from the shape AABB. */
void  jce_bullet_body_set_ccd(JceBulletWorld *bw, uint32_t idx,
                              float motion_threshold,
                              float swept_sphere_radius);
float jce_bullet_body_get_ccd_motion_threshold(JceBulletWorld *bw, uint32_t idx);
float jce_bullet_body_get_ccd_swept_sphere_radius(JceBulletWorld *bw, uint32_t idx);

/* Computed default swept-sphere radius for the body's current shape.
 * Returns 0.0f if idx invalid. */
float jce_bullet_body_compute_auto_swept_radius(JceBulletWorld *bw, uint32_t idx);

/* ================================================================== */
/* Constraints                                                         */
/* ================================================================== */

uint32_t jce_bullet_constraint_create(JceBulletWorld *bw,
                                       uint8_t type,
                                       uint32_t body_a, uint32_t body_b,
                                       jce_vec3 pivot_a, jce_vec3 pivot_b,
                                       jce_vec3 axis,
                                       float lower, float upper,
                                       bool disable_collision);
void jce_bullet_constraint_destroy(JceBulletWorld *bw, uint32_t idx);
void jce_bullet_constraint_set_limits(JceBulletWorld *bw, uint32_t idx,
                                       float lower, float upper);

/* Configurable joint (Unity-style per-axis 6DOF).  Stored in the SAME
 * constraint registry as jce_bullet_constraint_create; the returned slot is
 * destroyed via jce_bullet_constraint_destroy.  lin_motion/ang_motion entries
 * are 0=locked 1=limited 2=free; limits are symmetric (±linear_limit metres,
 * ±angular_limit_rad[axis] radians). */
uint32_t jce_bullet_configurable_joint_create(JceBulletWorld *bw,
                                               uint32_t body_a, uint32_t body_b,
                                               jce_vec3 anchor_a,
                                               jce_vec3 anchor_b,
                                               const int lin_motion[3],
                                               const int ang_motion[3],
                                               float linear_limit,
                                               const float angular_limit_rad[3],
                                               bool disable_collision);

/* Drive a HINGE or SLIDER at a target velocity.  Units are the AUTHOR'S --
 * rad/s + N*m for a hinge, m/s + N for a slider -- and the conversion to what
 * each Bullet class actually wants happens inside, because Bullet is not
 * consistent: a hinge takes a maximum IMPULSE while a slider and a 6DOF limit
 * motor take a force the solver divides by the step rate itself.  `fixed_dt`
 * is the world's fixed timestep, which is what turns a torque into that
 * impulse.  A slot that holds neither a hinge nor a slider is a no-op. */
void jce_bullet_constraint_set_motor(JceBulletWorld *bw, uint32_t idx,
                                      bool enabled, float target_velocity,
                                      float max_force, float fixed_dt);

/* Drive ONE axis of a configurable joint (slot must hold a 6DOF constraint).
 * `axis` is 0..2 linear X/Y/Z, 3..5 angular X/Y/Z -- Bullet's own numbering,
 * which is also the order JceConfigurableJointDesc's arrays use.
 * `mode` is JceJointDriveMode: 0 off, 1 velocity, 2 spring.  A no-op for any
 * other slot contents or an axis outside 0..5. */
void jce_bullet_configurable_joint_set_drive(JceBulletWorld *bw, uint32_t idx,
                                              int axis, int mode, float target,
                                              float spring, float damper,
                                              float max_force);

/* Last-step applied-impulse magnitude of constraint slot `idx`, or 0 when the
 * slot is dead/invalid. */
float jce_bullet_constraint_applied_impulse(JceBulletWorld *bw, uint32_t idx);
/* Applied TORQUE magnitude on body A, from the constraint's joint feedback.
 * Separate from the impulse query because getAppliedImpulse() has no angular
 * component to separate -- see the definition. */
float jce_bullet_constraint_applied_torque(JceBulletWorld *bw, uint32_t idx);

/* ================================================================== */
/* Joint introspection (P3-C.6 — editor gizmo)                         */
/* ================================================================== */

/* Plain-C mirror of JcePhysicsJointInfo so this internal header keeps
 * its zero-dependency stance on physics public headers.  Field order
 * matches one-for-one; jce_physics_joint_query.c copies between them. */
typedef struct {
    uint8_t  kind;            /* JcePhysicsJointKind */
    uint32_t body_a;
    uint32_t body_b;          /* UINT32_MAX = world anchor */
    jce_vec3 anchor_a;        /* world space */
    jce_vec3 anchor_b;        /* world space */
    jce_vec3 axis;            /* world space, normalised */
    float    limit_low;
    float    limit_high;
    jce_vec3 linear_lower;
    jce_vec3 linear_upper;
    jce_vec3 angular_lower;
    jce_vec3 angular_upper;
} JceBulletJointInfo;

/* Find the first constraint touching `body_idx` and fill `out`.  v1
 * returns the lowest-index match only.  Returns false if the body
 * has no constraint or any argument is invalid. */
bool jce_bullet_joint_get_info_for_body(JceBulletWorld *bw,
                                        uint32_t body_idx,
                                        JceBulletJointInfo *out);

/* ================================================================== */
/* Character controller                                                */
/* ================================================================== */

/* `col_group` / `col_mask` are the broadphase filter pair, exactly as for
 * bodies -- this layer stays layer-matrix-agnostic and the caller resolves
 * them.  They govern the capsule itself AND the ground / step / head-clearance
 * probes, which must agree with it or the character stands on a surface it
 * cannot collide with (or the reverse). */
uint32_t jce_bullet_character_create(JceBulletWorld *bw,
                                      jce_vec3 pos, float radius,
                                      float height, float step_height,
                                      float max_slope_rad,
                                      float gravity, float jump_speed,
                                      float accel, float air_control,
                                      uint32_t col_group, uint32_t col_mask);
void jce_bullet_character_destroy(JceBulletWorld *bw, uint32_t idx);
void jce_bullet_character_move(JceBulletWorld *bw, uint32_t idx,
                                jce_vec3 walk_dir, float dt);
bool jce_bullet_character_jump(JceBulletWorld *bw, uint32_t idx);
void jce_bullet_character_get_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *pos);
void jce_bullet_character_set_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 pos);
bool jce_bullet_character_is_grounded(JceBulletWorld *bw, uint32_t idx);
void jce_bullet_character_get_velocity(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *out_vel);
void jce_bullet_character_cut_jump(JceBulletWorld *bw, uint32_t idx,
                                    float factor);

/* ================================================================== */
/* Vehicle controller                                                  */
/* ================================================================== */

uint32_t jce_bullet_vehicle_create(JceBulletWorld *bw,
                                    jce_vec3 pos, jce_quat rot,
                                    jce_vec3 chassis_half_ext,
                                    float chassis_mass,
                                    float max_engine_force,
                                    float max_brake_force,
                                    float max_steering_rad,
                                    uint32_t col_group, uint32_t col_mask);

void jce_bullet_vehicle_destroy(JceBulletWorld *bw, uint32_t idx);

uint32_t jce_bullet_vehicle_add_wheel(JceBulletWorld *bw, uint32_t idx,
                                       jce_vec3 connection,
                                       jce_vec3 wheel_dir,
                                       jce_vec3 wheel_axle,
                                       float suspension_rest_len,
                                       float wheel_radius,
                                       bool is_front,
                                       float susp_stiffness,
                                       float susp_damping,
                                       float susp_compression,
                                       float friction_slip,
                                       float roll_influence);

void jce_bullet_vehicle_set_input(JceBulletWorld *bw, uint32_t idx,
                                   float throttle, float brake, float steer);
void jce_bullet_vehicle_add_wheel_input(JceBulletWorld *bw, uint32_t idx,
                                        uint32_t wheel, float engine_force,
                                        float brake_force, float steer_rad);

void jce_bullet_vehicle_get_chassis_transform(JceBulletWorld *bw, uint32_t idx,
                                                jce_vec3 *pos, jce_quat *rot);

void jce_bullet_vehicle_get_wheel_transform(JceBulletWorld *bw, uint32_t idx,
                                              uint32_t wheel,
                                              jce_vec3 *pos, jce_quat *rot);

float jce_bullet_vehicle_get_speed(JceBulletWorld *bw, uint32_t idx);

/* ================================================================== */
/* P3-C.4 (cloth): native rigid-body pointer accessor + default world  */
/* lookup.  Cloth lives in its own secondary btSoftRigidDynamicsWorld  */
/* and needs a btRigidBody* to anchor against the primary rigid world. */
/* ================================================================== */

/* Returns the underlying btRigidBody* (opaque void*) for `idx`, or
 * NULL on invalid idx / dead slot. */
void *jce_bullet_body_get_rigid_native_(JceBulletWorld *bw, uint32_t idx);

/* The most recently created (non-destroyed) JceBulletWorld — used by
 * the cloth module's anchor helper.  Returns NULL if no rigid world
 * is alive.  Updated by jce_physics_create / jce_physics_destroy. */
JceBulletWorld *jce_physics_default_bullet_world_(void);

/* Setter used internally by jce_physics_create / jce_physics_destroy. */
void jce_physics_set_default_bullet_world_(JceBulletWorld *bw);

/* Install the Bullet->jce allocator hook (btAlignedAllocSetCustom*) exactly
 * once.  MUST be called before ANY Bullet object is allocated in a TU, or
 * that object's memory comes from the CRT allocator and is later freed via
 * mi_free = cross-allocator free / heap corruption.  Idempotent (internal
 * latch); Bullet forbids swapping allocators once allocations exist, so the
 * latch must never be reset.  Owned by jce_physics_bullet.cpp; the cloth
 * module (separate TU, secondary soft world) calls it in ensure_world. */
void jce_bullet_install_allocator_(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PHYSICS_INTERNAL_H */
