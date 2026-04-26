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

#ifdef __cplusplus
}
#endif

#endif /* JCE_PHYSICS_INTERNAL_H */
