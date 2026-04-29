/*
 * jce_steering.c — Reynolds-style steering behavior implementations.
 */
#include <jce/middleware/ai/jce_steering.h>
#include <math.h>

/* ----- Internal helpers ------------------------------------------- */

static jce_vec3 desired_minus_current(jce_vec3 desired_dir, float max_speed,
                                      jce_vec3 vel)
{
    jce_vec3 desired = jce_v3_scale(desired_dir, max_speed);
    return jce_v3_sub(desired, vel);
}

static float v3_len_sq(jce_vec3 v)
{
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

/* xorshift32 — deterministic per-agent randomness. */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static float frand(uint32_t *s) { return (float)(xs32(s) & 0xFFFFFF) / (float)0x1000000; }
static float frand_signed(uint32_t *s) { return frand(s) * 2.0f - 1.0f; }

/* ================================================================== */

jce_vec3 jce_steer_seek(jce_vec3 pos, jce_vec3 vel, jce_vec3 target, float max_speed)
{
    jce_vec3 to = jce_v3_sub(target, pos);
    if (v3_len_sq(to) < 1e-8f) return jce_v3(0, 0, 0);
    return desired_minus_current(jce_v3_normalize(to), max_speed, vel);
}

jce_vec3 jce_steer_flee(jce_vec3 pos, jce_vec3 vel, jce_vec3 threat, float max_speed)
{
    jce_vec3 away = jce_v3_sub(pos, threat);
    if (v3_len_sq(away) < 1e-8f) return jce_v3(0, 0, 0);
    return desired_minus_current(jce_v3_normalize(away), max_speed, vel);
}

jce_vec3 jce_steer_arrive(jce_vec3 pos, jce_vec3 vel, jce_vec3 target,
                          float max_speed, float slowing_radius)
{
    jce_vec3 to = jce_v3_sub(target, pos);
    float d = jce_v3_len(to);
    if (d < 1e-4f) return jce_v3_negate(vel); /* brake */
    float speed = max_speed;
    if (slowing_radius > 1e-4f && d < slowing_radius)
        speed = max_speed * (d / slowing_radius);
    jce_vec3 desired = jce_v3_scale(to, speed / d);
    return jce_v3_sub(desired, vel);
}

jce_vec3 jce_steer_pursue(jce_vec3 pos, jce_vec3 vel,
                          jce_vec3 target_pos, jce_vec3 target_vel,
                          float max_speed)
{
    jce_vec3 to = jce_v3_sub(target_pos, pos);
    float dist = jce_v3_len(to);
    float my_speed = jce_v3_len(vel);
    /* Predict lead time inversely proportional to relative speed. */
    float ahead = (my_speed > 1e-4f) ? (dist / (my_speed + jce_v3_len(target_vel) + 1e-4f)) : 0.0f;
    jce_vec3 future = jce_v3_add(target_pos, jce_v3_scale(target_vel, ahead));
    return jce_steer_seek(pos, vel, future, max_speed);
}

jce_vec3 jce_steer_evade(jce_vec3 pos, jce_vec3 vel,
                         jce_vec3 threat_pos, jce_vec3 threat_vel,
                         float max_speed)
{
    jce_vec3 to = jce_v3_sub(threat_pos, pos);
    float dist = jce_v3_len(to);
    float my_speed = jce_v3_len(vel);
    float ahead = (my_speed > 1e-4f) ? (dist / (my_speed + jce_v3_len(threat_vel) + 1e-4f)) : 0.0f;
    jce_vec3 future = jce_v3_add(threat_pos, jce_v3_scale(threat_vel, ahead));
    return jce_steer_flee(pos, vel, future, max_speed);
}

jce_vec3 jce_steer_wander(jce_vec3 pos, jce_vec3 vel,
                          JceSteerWander *state,
                          float circle_distance, float circle_radius,
                          float angle_change_per_sec, float dt,
                          float max_speed)
{
    if (!state) return jce_v3(0, 0, 0);
    state->angle += frand_signed(&state->seed) * angle_change_per_sec * dt;

    /* Forward direction; if velocity is zero, fall back to +X. */
    jce_vec3 forward = (v3_len_sq(vel) > 1e-6f) ? jce_v3_normalize(vel) : jce_v3(1, 0, 0);

    /* Pick a side vector perpendicular to forward in the world XZ plane
     * (so wander stays planar — typical for ground agents).  Boats/drones
     * can rotate the result if they want full 3D freedom. */
    jce_vec3 side = jce_v3_normalize(jce_v3_cross(forward, jce_v3(0, 1, 0)));
    if (v3_len_sq(side) < 1e-6f) side = jce_v3(0, 0, 1);

    jce_vec3 circle_center = jce_v3_add(pos, jce_v3_scale(forward, circle_distance));
    jce_vec3 displacement  = jce_v3_add(jce_v3_scale(forward, cosf(state->angle) * circle_radius),
                                         jce_v3_scale(side,    sinf(state->angle) * circle_radius));
    jce_vec3 target = jce_v3_add(circle_center, displacement);
    return jce_steer_seek(pos, vel, target, max_speed);
}

jce_vec3 jce_steer_path_follow(jce_vec3 pos, jce_vec3 vel,
                               const jce_vec3 *waypoints, uint32_t count,
                               uint32_t *idx, float arrive_radius,
                               float max_speed, bool *out_finished)
{
    if (out_finished) *out_finished = false;
    if (!waypoints || !idx || count == 0) {
        if (out_finished) *out_finished = true;
        return jce_v3(0, 0, 0);
    }
    if (*idx >= count) {
        if (out_finished) *out_finished = true;
        return jce_v3(0, 0, 0);
    }
    jce_vec3 target = waypoints[*idx];
    jce_vec3 to = jce_v3_sub(target, pos);
    if (v3_len_sq(to) < arrive_radius * arrive_radius) {
        (*idx)++;
        if (*idx >= count) {
            if (out_finished) *out_finished = true;
            /* Slowdown on final waypoint. */
            return jce_steer_arrive(pos, vel, target, max_speed, arrive_radius * 2.0f);
        }
        target = waypoints[*idx];
    }
    /* Arrive on the *last* segment for smooth stopping; pure seek otherwise. */
    if (*idx == count - 1)
        return jce_steer_arrive(pos, vel, target, max_speed, arrive_radius * 4.0f);
    return jce_steer_seek(pos, vel, target, max_speed);
}

jce_vec3 jce_steer_avoid_spheres(jce_vec3 pos, jce_vec3 vel,
                                 const JceSteerSphere *obstacles,
                                 uint32_t count, float look_ahead,
                                 float max_speed)
{
    if (!obstacles || count == 0) return jce_v3(0, 0, 0);
    float vlen = jce_v3_len(vel);
    if (vlen < 1e-4f) return jce_v3(0, 0, 0);
    jce_vec3 ahead_dir = jce_v3_scale(vel, 1.0f / vlen);
    jce_vec3 ahead = jce_v3_add(pos, jce_v3_scale(ahead_dir, look_ahead));

    /* Find the closest sphere whose surface intersects the look-ahead segment. */
    int best = -1;
    float best_d = 1e30f;
    for (uint32_t i = 0; i < count; ++i) {
        jce_vec3 to_c = jce_v3_sub(obstacles[i].center, pos);
        float fwd = jce_v3_dot(to_c, ahead_dir);
        if (fwd < -obstacles[i].radius || fwd > look_ahead + obstacles[i].radius) continue;
        jce_vec3 closest = jce_v3_add(pos, jce_v3_scale(ahead_dir, fwd));
        float gap = jce_v3_len(jce_v3_sub(obstacles[i].center, closest));
        if (gap < obstacles[i].radius && fwd < best_d) {
            best_d = fwd;
            best = (int)i;
        }
    }
    if (best < 0) return jce_v3(0, 0, 0);
    /* Push laterally away from the obstacle center. */
    jce_vec3 push = jce_v3_sub(ahead, obstacles[best].center);
    if (v3_len_sq(push) < 1e-6f) {
        /* Tie-break: cross with up. */
        push = jce_v3_cross(ahead_dir, jce_v3(0, 1, 0));
    }
    return jce_v3_scale(jce_v3_normalize(push), max_speed);
}

/* ================================================================== */
/* Flocking                                                            */
/* ================================================================== */

jce_vec3 jce_steer_separation(jce_vec3 pos,
                              const jce_vec3 *neighbors, uint32_t count,
                              float desired_separation)
{
    jce_vec3 sum = jce_v3(0, 0, 0);
    uint32_t n = 0;
    if (!neighbors) return sum;
    for (uint32_t i = 0; i < count; ++i) {
        jce_vec3 d = jce_v3_sub(pos, neighbors[i]);
        float dist = jce_v3_len(d);
        if (dist > 1e-4f && dist < desired_separation) {
            sum = jce_v3_add(sum, jce_v3_scale(d, 1.0f / (dist * dist)));
            n++;
        }
    }
    if (n == 0) return jce_v3(0, 0, 0);
    return sum;
}

jce_vec3 jce_steer_alignment(jce_vec3 vel,
                             const jce_vec3 *neighbor_velocities,
                             uint32_t count, float max_speed)
{
    if (!neighbor_velocities || count == 0) return jce_v3(0, 0, 0);
    jce_vec3 avg = jce_v3(0, 0, 0);
    for (uint32_t i = 0; i < count; ++i)
        avg = jce_v3_add(avg, neighbor_velocities[i]);
    avg = jce_v3_scale(avg, 1.0f / (float)count);
    if (v3_len_sq(avg) < 1e-6f) return jce_v3(0, 0, 0);
    return jce_v3_sub(jce_v3_scale(jce_v3_normalize(avg), max_speed), vel);
}

jce_vec3 jce_steer_cohesion(jce_vec3 pos, jce_vec3 vel,
                            const jce_vec3 *neighbor_positions,
                            uint32_t count, float max_speed)
{
    if (!neighbor_positions || count == 0) return jce_v3(0, 0, 0);
    jce_vec3 center = jce_v3(0, 0, 0);
    for (uint32_t i = 0; i < count; ++i)
        center = jce_v3_add(center, neighbor_positions[i]);
    center = jce_v3_scale(center, 1.0f / (float)count);
    return jce_steer_seek(pos, vel, center, max_speed);
}

jce_vec3 jce_steer_truncate(jce_vec3 v, float max_len)
{
    float lsq = v3_len_sq(v);
    if (lsq <= max_len * max_len) return v;
    float l = sqrtf(lsq);
    if (l < 1e-6f) return jce_v3(0, 0, 0);
    return jce_v3_scale(v, max_len / l);
}
