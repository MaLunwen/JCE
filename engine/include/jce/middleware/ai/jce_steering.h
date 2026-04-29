/*
 * jce_steering.h  Generic steering behaviors (Reynolds-style).
 *
 * Pure-math, allocation-free building blocks for autonomous agents.
 * Returns a desired *steering force* (acceleration in m/s^2 if mass=1)
 * that callers integrate however they want.  No game-specific concepts
 * (vehicle/ped/etc.) — works for crowds, fish, drones, anything that
 * has position+velocity in 3D.
 *
 * Usage:
 *   jce_vec3 force = jce_steer_seek(agent, target, max_speed);
 *   force = jce_steer_clamp(force, max_force);
 *   agent->velocity = jce_v3_add(agent->velocity, jce_v3_scale(force, dt));
 *   agent->velocity = jce_steer_truncate(agent->velocity, max_speed);
 *   agent->position = jce_v3_add(agent->position, jce_v3_scale(agent->velocity, dt));
 *
 * Thread-safety: pure functions on caller-owned data — fully
 * re-entrant; safe to call from any thread, on any agent, in parallel.
 * No internal state.
 *
 * Example:
 *   JceSteerAgent agent = { pos, vel, mass, max_speed, max_force, radius };
 *   jce_vec3 wander = jce_steer_wander(&agent, dt);
 *   jce_vec3 sep    = jce_steer_separation(&agent, neighbors, n_count, sep_radius);
 *   jce_vec3 force  = jce_v3_add(jce_v3_scale(wander, 0.5f),
 *                                jce_v3_scale(sep,    1.5f));
 *   force = jce_steer_clamp(force, agent.max_force);
 *   agent.velocity = jce_steer_truncate(
 *       jce_v3_add(agent.velocity, jce_v3_scale(force, dt)), agent.max_speed);
 *   agent.position = jce_v3_add(agent.position,
 *                               jce_v3_scale(agent.velocity, dt));
 *
 * Layer: AI (Layer 3).  Header-light; one .c for non-trivial pieces.
 */
#ifndef JCE_STEERING_H
#define JCE_STEERING_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Agent state expected by combination behaviors.                      */
/* Callers may use their own struct — these helpers take primitives.   */
/* ================================================================== */
typedef struct {
    jce_vec3 position;
    jce_vec3 velocity;
    float    max_speed;   /* m/s */
    float    max_force;   /* m/s^2 (treats mass = 1) */
} JceSteerAgent;

/* ================================================================== */
/* Single-target behaviors                                             */
/* ================================================================== */

/* Seek: steer straight at target at full speed. */
JCE_API jce_vec3 jce_steer_seek(jce_vec3 pos, jce_vec3 vel,
                                jce_vec3 target, float max_speed);

/* Flee: opposite of seek. */
JCE_API jce_vec3 jce_steer_flee(jce_vec3 pos, jce_vec3 vel,
                                jce_vec3 threat, float max_speed);

/* Arrive: like seek but decelerates within slowing_radius of target. */
JCE_API jce_vec3 jce_steer_arrive(jce_vec3 pos, jce_vec3 vel,
                                  jce_vec3 target, float max_speed,
                                  float slowing_radius);

/* Pursue: lead a moving target by predicting its future position. */
JCE_API jce_vec3 jce_steer_pursue(jce_vec3 pos, jce_vec3 vel,
                                  jce_vec3 target_pos, jce_vec3 target_vel,
                                  float max_speed);

/* Evade: opposite of pursue. */
JCE_API jce_vec3 jce_steer_evade(jce_vec3 pos, jce_vec3 vel,
                                 jce_vec3 threat_pos, jce_vec3 threat_vel,
                                 float max_speed);

/* ================================================================== */
/* Wander                                                              */
/* ================================================================== */
/* Wander state — one per agent, persistent across frames. */
typedef struct {
    float angle;   /* radians, accumulated */
    uint32_t seed; /* xorshift */
} JceSteerWander;

/* Project a point on a circle in front of the agent and randomly walk
 * its angle each frame to produce a smooth, naturalistic wander.
 *
 *   circle_distance — how far ahead the wander circle sits
 *   circle_radius   — radius of the wander circle
 *   angle_change    — max radians/sec angular jitter
 */
JCE_API jce_vec3 jce_steer_wander(jce_vec3 pos, jce_vec3 vel,
                                  JceSteerWander *state,
                                  float circle_distance,
                                  float circle_radius,
                                  float angle_change_per_sec,
                                  float dt,
                                  float max_speed);

/* ================================================================== */
/* Path following                                                      */
/* ================================================================== */
/* Follow a polyline by aiming at the next waypoint within `radius`,
 * advancing the index when reached.  Returns SUCCESS once index == count.
 * `*idx` is updated in place. */
JCE_API jce_vec3 jce_steer_path_follow(jce_vec3 pos, jce_vec3 vel,
                                       const jce_vec3 *waypoints,
                                       uint32_t waypoint_count,
                                       uint32_t *idx,
                                       float arrive_radius,
                                       float max_speed,
                                       bool *out_finished);

/* ================================================================== */
/* Obstacle avoidance                                                  */
/* ================================================================== */
/* Sphere-cast forward; if any sphere obstacle in the array intersects
 * the look-ahead probe, steer perpendicular away from it.  Returns
 * zero vector if nothing in the way. */
typedef struct {
    jce_vec3 center;
    float    radius;
} JceSteerSphere;

JCE_API jce_vec3 jce_steer_avoid_spheres(jce_vec3 pos, jce_vec3 vel,
                                         const JceSteerSphere *obstacles,
                                         uint32_t obstacle_count,
                                         float look_ahead_distance,
                                         float max_speed);

/* ================================================================== */
/* Flocking (Reynolds boids)                                           */
/* ================================================================== */
/* Caller supplies a flat array of neighbor states (positions+velocities).
 * `count` is the *neighbor* count (not including self).
 *
 * The three classic forces — separation pushes away from too-close
 * neighbors, alignment matches average heading, cohesion pulls toward
 * group center.  Combine with weights per agent personality.
 */

/* Separation: 1/distance falloff away from each too-close neighbor. */
JCE_API jce_vec3 jce_steer_separation(jce_vec3 pos,
                                      const jce_vec3 *neighbor_positions,
                                      uint32_t count,
                                      float desired_separation);

/* Alignment: steer toward average velocity of neighbors. */
JCE_API jce_vec3 jce_steer_alignment(jce_vec3 vel,
                                     const jce_vec3 *neighbor_velocities,
                                     uint32_t count,
                                     float max_speed);

/* Cohesion: steer toward center-of-mass of neighbors. */
JCE_API jce_vec3 jce_steer_cohesion(jce_vec3 pos, jce_vec3 vel,
                                    const jce_vec3 *neighbor_positions,
                                    uint32_t count,
                                    float max_speed);

/* ================================================================== */
/* Utility                                                             */
/* ================================================================== */
/* Truncate vector length to `max` (returns same vector if shorter). */
JCE_API jce_vec3 jce_steer_truncate(jce_vec3 v, float max_len);

/* Same as truncate but renamed for clarity when used on force vector. */
static inline jce_vec3 jce_steer_clamp(jce_vec3 v, float max_force) {
    return jce_steer_truncate(v, max_force);
}

JCE_EXTERN_C_END
#endif /* JCE_STEERING_H */
