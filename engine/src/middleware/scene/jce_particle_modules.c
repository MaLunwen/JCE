/*
 * jce_particle_modules.c  Particle-emitter module evaluators.
 *
 * Linear search the gradient stops / curve keys is fine — both lists
 * are capped at 8 entries by the component definition.
 */

#include <jce/middleware/scene/jce_particle_modules.h>

#include <math.h>

#ifndef JCE_PI
#define JCE_PI 3.14159265358979323846f
#endif

static float clamp01(float v)
{ return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool jce_particle_sample_color_over_lifetime(
    const JceParticleEmitterComponent *e, float t, float out[4])
{
    if (!e || !out) return false;
    if (!e->color_over_lifetime_enabled) return false;
    if (e->color_stop_count <= 0) return false;
    t = clamp01(t);

    /* Single stop → constant color. */
    if (e->color_stop_count == 1) {
        out[0] = e->color_stops[0].color[0];
        out[1] = e->color_stops[0].color[1];
        out[2] = e->color_stops[0].color[2];
        out[3] = e->color_stops[0].color[3];
        return true;
    }

    /* Find bracketing stops. */
    int lo = 0, hi = e->color_stop_count - 1;
    if (t <= e->color_stops[0].position) {
        out[0] = e->color_stops[0].color[0];
        out[1] = e->color_stops[0].color[1];
        out[2] = e->color_stops[0].color[2];
        out[3] = e->color_stops[0].color[3];
        return true;
    }
    if (t >= e->color_stops[hi].position) {
        out[0] = e->color_stops[hi].color[0];
        out[1] = e->color_stops[hi].color[1];
        out[2] = e->color_stops[hi].color[2];
        out[3] = e->color_stops[hi].color[3];
        return true;
    }
    for (int i = 0; i < e->color_stop_count - 1; ++i) {
        float p0 = e->color_stops[i].position;
        float p1 = e->color_stops[i + 1].position;
        if (t >= p0 && t <= p1) { lo = i; hi = i + 1; break; }
    }
    float p0 = e->color_stops[lo].position;
    float p1 = e->color_stops[hi].position;
    float u = (p1 > p0) ? (t - p0) / (p1 - p0) : 0.0f;
    for (int c = 0; c < 4; ++c) {
        float a = e->color_stops[lo].color[c];
        float b = e->color_stops[hi].color[c];
        out[c] = a + (b - a) * u;
    }
    return true;
}

bool jce_particle_sample_size_over_lifetime(
    const JceParticleEmitterComponent *e, float t, float *out)
{
    if (!e || !out) return false;
    if (!e->size_over_lifetime_enabled) return false;
    if (e->size_curve_count <= 0) return false;
    t = clamp01(t);
    if (e->size_curve_count == 1) { *out = e->size_curve[0].value; return true; }

    int hi = e->size_curve_count - 1;
    if (t <= e->size_curve[0].time)  { *out = e->size_curve[0].value;  return true; }
    if (t >= e->size_curve[hi].time) { *out = e->size_curve[hi].value; return true; }
    int lo_i = 0, hi_i = hi;
    for (int i = 0; i < e->size_curve_count - 1; ++i) {
        float a = e->size_curve[i].time;
        float b = e->size_curve[i + 1].time;
        if (t >= a && t <= b) { lo_i = i; hi_i = i + 1; break; }
    }
    float a = e->size_curve[lo_i].time;
    float b = e->size_curve[hi_i].time;
    float u = (b > a) ? (t - a) / (b - a) : 0.0f;
    *out = e->size_curve[lo_i].value
         + (e->size_curve[hi_i].value - e->size_curve[lo_i].value) * u;
    return true;
}

/* ── Spawn position by shape ─────────────────────────────────────── */

jce_vec3 jce_particle_sample_spawn_position(
    const JceParticleEmitterComponent *e,
    float rx, float ry, float rz)
{
    jce_vec3 zero = {0, 0, 0};
    if (!e) return zero;
    rx = clamp01(rx);
    ry = clamp01(ry);
    rz = clamp01(rz);

    switch (e->shape) {
        case JCE_PARTICLE_SHAPE_POINT:
            return zero;
        case JCE_PARTICLE_SHAPE_SPHERE: {
            /* Uniform inside a sphere using cube-root for radial
             * distance; spherical coords from rx/ry. */
            float theta = rx * JCE_PI * 2.0f;
            float phi   = acosf(2.0f * ry - 1.0f);
            float r     = e->shape_radius * cbrtf(rz);
            jce_vec3 p;
            p.x = r * sinf(phi) * cosf(theta);
            p.y = r * sinf(phi) * sinf(theta);
            p.z = r * cosf(phi);
            return p;
        }
        case JCE_PARTICLE_SHAPE_BOX: {
            jce_vec3 p;
            p.x = (rx * 2.0f - 1.0f) * e->shape_size[0];
            p.y = (ry * 2.0f - 1.0f) * e->shape_size[1];
            p.z = (rz * 2.0f - 1.0f) * e->shape_size[2];
            return p;
        }
        case JCE_PARTICLE_SHAPE_CONE: {
            /* Spawn on the cone's BASE disc; height grows along +Z
             * but we leave Z=0 here so the velocity module can drive
             * outward motion. */
            float angle  = rx * JCE_PI * 2.0f;
            float radius = e->shape_radius * sqrtf(ry);
            jce_vec3 p;
            p.x = cosf(angle) * radius;
            p.y = sinf(angle) * radius;
            p.z = 0.0f;
            return p;
        }
        default: return zero;
    }
}

jce_vec3 jce_particle_sample_initial_velocity(
    const JceParticleEmitterComponent *e, jce_vec3 spawn)
{
    jce_vec3 v = {0, 0, 0};
    if (!e) return v;
    /* Cone shapes imply outward + forward velocity:
     *   direction = normalise(spawn.xy + cone_height * +Z) */
    if (e->shape == JCE_PARTICLE_SHAPE_CONE) {
        float h = e->shape_size[2];
        if (h <= 0.0f) h = 1.0f;
        float ang = e->shape_angle_deg * (JCE_PI / 180.0f);
        float scale = tanf(ang) * h;
        v.x = spawn.x;
        v.y = spawn.y;
        v.z = h;
        /* Normalise then re-scale by cone height so faster particles
         * leave farther — common artist expectation. */
        float lsq = v.x*v.x + v.y*v.y + v.z*v.z;
        if (lsq > 0.0f) {
            float inv = 1.0f / sqrtf(lsq);
            v.x *= inv; v.y *= inv; v.z *= inv;
        }
        v.x *= scale * 0.5f;
        v.y *= scale * 0.5f;
        v.z *= h;
    }
    /* Optional override from velocity_over_lifetime (treated as
     * additive bias when enabled). */
    if (e->velocity_over_lifetime_enabled) {
        v.x += e->velocity_over_lifetime[0];
        v.y += e->velocity_over_lifetime[1];
        v.z += e->velocity_over_lifetime[2];
    }
    return v;
}
