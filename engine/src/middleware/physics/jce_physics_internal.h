/*
 * jce_physics_internal.h  Internal bridge between C99 front-end and
 *                         C++ Bullet3 back-end.
 *
 * All functions use the jce_bullet_* prefix.  They are implemented in
 * jce_physics_bullet.cpp and called exclusively from jce_physics.c.
 */

#ifndef JCE_PHYSICS_INTERNAL_H
#define JCE_PHYSICS_INTERNAL_H

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

JceBulletWorld *jce_bullet_create(jce_vec3 gravity, uint32_t max_bodies);
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
                                uint16_t col_group, uint16_t col_mask,
                                bool is_trigger);

void jce_bullet_body_destroy(JceBulletWorld *bw, uint32_t idx);

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
void jce_bullet_body_get_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 *vel);
void jce_bullet_body_set_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 vel);

/* ================================================================== */
/* Forces & impulses                                                   */
/* ================================================================== */

void jce_bullet_body_apply_force(JceBulletWorld *bw, uint32_t idx,
                                 jce_vec3 force);
void jce_bullet_body_apply_impulse(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 impulse);
void jce_bullet_body_apply_torque(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 torque);

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

/* ================================================================== */
/* Overlap queries (Unity-style Physics.Overlap*)                      */
/* ================================================================== */

/* shape: 0=sphere(half_ext.x=radius), 1=box(half_ext), 2=capsule(x=radius,y=halfH).
 * Writes up to `cap` body indices to out_bodies; returns count actually
 * written.  collision_mask filters which groups are considered. */
uint32_t jce_bullet_overlap_shape(JceBulletWorld *bw,
                                  uint8_t shape, jce_vec3 center,
                                  jce_quat rot, jce_vec3 half_ext,
                                  uint16_t collision_mask,
                                  uint32_t *out_bodies, uint32_t cap);

/* Convex sweep — returns first hit along `dir` for `max_dist`.  Same
 * shape encoding as overlap_shape.  Result.body_idx == UINT32_MAX iff no
 * hit. */
JceBulletRayResult jce_bullet_shape_cast(JceBulletWorld *bw,
                                         uint8_t shape,
                                         jce_vec3 origin, jce_quat rot,
                                         jce_vec3 half_ext,
                                         jce_vec3 dir, float max_dist);

/* ================================================================== */
/* Contact callbacks                                                   */
/* ================================================================== */

typedef void (*jce_bullet_contact_fn)(uint32_t body_a, uint32_t body_b,
                                      const float normal[3],
                                      const float point[3],
                                      float depth, void *ud);

void jce_bullet_set_contact_begin(JceBulletWorld *bw,
                                  jce_bullet_contact_fn fn, void *ud);
void jce_bullet_set_contact_end(JceBulletWorld *bw,
                                jce_bullet_contact_fn fn, void *ud);

/* ================================================================== */
/* Utility                                                             */
/* ================================================================== */

uint32_t jce_bullet_body_count(JceBulletWorld *bw);

/* ================================================================== */
/* Collision filter                                                    */
/* ================================================================== */

void jce_bullet_body_set_collision_filter(JceBulletWorld *bw, uint32_t idx,
                                          uint16_t group, uint16_t mask);

/* Continuous collision detection.  Pass 0 for both to disable. */
void jce_bullet_body_set_ccd(JceBulletWorld *bw, uint32_t idx,
                             float motion_threshold, float swept_radius);

/* Create a STATIC triangle-mesh body via btBvhTriangleMeshShape.
 * Returns the body index, or UINT32_MAX on failure (pool exhausted /
 * empty mesh).  Bullet retains pointers into the supplied vertex/index
 * arrays — the bridge copies them into a heap buffer owned by the
 * world so the caller can free its inputs. */
uint32_t jce_bullet_body_create_static_mesh(JceBulletWorld *bw,
                                             jce_vec3 pos, jce_quat rot,
                                             const float    *vertices,
                                             uint32_t        vertex_count,
                                             const uint32_t *indices,
                                             uint32_t        triangle_count,
                                             float friction, float restitution,
                                             uint16_t col_group, uint16_t col_mask,
                                             bool is_trigger);

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

/* ================================================================== */
/* Character controller                                                */
/* ================================================================== */

uint32_t jce_bullet_character_create(JceBulletWorld *bw,
                                      jce_vec3 pos, float radius,
                                      float height, float step_height,
                                      float max_slope_rad,
                                      float gravity, float jump_speed);
void jce_bullet_character_destroy(JceBulletWorld *bw, uint32_t idx);
void jce_bullet_character_move(JceBulletWorld *bw, uint32_t idx,
                                jce_vec3 walk_dir, float dt);
void jce_bullet_character_jump(JceBulletWorld *bw, uint32_t idx);
void jce_bullet_character_get_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *pos);
bool jce_bullet_character_is_grounded(JceBulletWorld *bw, uint32_t idx);

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
                                    uint16_t col_group, uint16_t col_mask);

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

void jce_bullet_vehicle_get_chassis_transform(JceBulletWorld *bw, uint32_t idx,
                                                jce_vec3 *pos, jce_quat *rot);

void jce_bullet_vehicle_get_wheel_transform(JceBulletWorld *bw, uint32_t idx,
                                              uint32_t wheel,
                                              jce_vec3 *pos, jce_quat *rot);

float jce_bullet_vehicle_get_speed(JceBulletWorld *bw, uint32_t idx);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PHYSICS_INTERNAL_H */
