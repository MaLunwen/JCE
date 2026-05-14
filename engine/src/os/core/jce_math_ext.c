/*
 * jce_math_ext.c  Unity-style scalar / vector / random / AABB helpers.
 *
 * All math is single-precision.  Random stream is a 64-bit LCG with
 * the same constants Numerical Recipes uses (Knuth's MMIX).  Seeded
 * lazily from jce_time_perf_counter so unconfigured callers still
 * get varying sequences across runs.
 */

#include <jce/os/core/jce_math_ext.h>
#include <jce/os/core/jce_timer.h>

#include <math.h>

#ifndef JCE_PI
#define JCE_PI 3.14159265358979323846f
#endif

/* ── Scalar ──────────────────────────────────────────────────── */

float jce_clamp(float v, float lo, float hi)
{ return v < lo ? lo : (v > hi ? hi : v); }

float jce_clamp01(float v)
{ return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float jce_repeat(float t, float length)
{
    if (length <= 0.0f) return 0.0f;
    /* Match Unity Mathf.Repeat: t - floor(t/length)*length. */
    float r = t - floorf(t / length) * length;
    if (r < 0.0f) r += length;
    if (r >= length) r = 0.0f;  /* defend against rounding */
    return r;
}

float jce_pingpong(float t, float length)
{
    if (length <= 0.0f) return 0.0f;
    t = jce_repeat(t, length * 2.0f);
    return length - fabsf(t - length);
}

float jce_smoothstep(float e0, float e1, float t)
{
    if (e1 <= e0) return t < e0 ? 0.0f : 1.0f;
    float x = jce_clamp01((t - e0) / (e1 - e0));
    return x * x * (3.0f - 2.0f * x);
}

float jce_remap(float v, float i0, float i1, float o0, float o1)
{
    if (i1 == i0) return o0;
    float t = (v - i0) / (i1 - i0);
    return o0 + t * (o1 - o0);
}

float jce_lerp_unclamped(float a, float b, float t)
{ return a + (b - a) * t; }

float jce_inverse_lerp(float a, float b, float v)
{
    if (b == a) return 0.0f;
    return jce_clamp01((v - a) / (b - a));
}

float jce_move_towards(float cur, float target, float max_delta)
{
    if (max_delta < 0.0f) max_delta = 0.0f;
    float d = target - cur;
    if (fabsf(d) <= max_delta) return target;
    return cur + (d > 0.0f ? max_delta : -max_delta);
}

float jce_smooth_damp(float cur, float target, float *vel,
                      float smooth_time, float max_speed, float dt)
{
    if (!vel) return target;
    if (smooth_time < 0.0001f) smooth_time = 0.0001f;
    if (dt <= 0.0f) return cur;

    /* Game Programming Gems 4 — Bobic's fast critically-damped spring. */
    float omega = 2.0f / smooth_time;
    float x = omega * dt;
    float exp_decay = 1.0f / (1.0f + x + 0.48f*x*x + 0.235f*x*x*x);

    float change = cur - target;
    float orig_target = target;

    /* Clamp max speed. */
    if (max_speed > 0.0f) {
        float max_change = max_speed * smooth_time;
        if (change >  max_change) change =  max_change;
        if (change < -max_change) change = -max_change;
    }
    target = cur - change;

    float temp = (*vel + omega * change) * dt;
    *vel = (*vel - omega * temp) * exp_decay;
    float result = target + (change + temp) * exp_decay;

    /* Prevent overshooting past original target. */
    if ((orig_target - cur > 0.0f) == (result > orig_target)) {
        result = orig_target;
        *vel = (result - orig_target) / dt;
    }
    return result;
}

/* ── Vector ──────────────────────────────────────────────────── */

jce_vec3 jce_v3_move_towards(jce_vec3 cur, jce_vec3 target, float max_delta)
{
    jce_vec3 d = { target.x - cur.x, target.y - cur.y, target.z - cur.z };
    float lsq = d.x*d.x + d.y*d.y + d.z*d.z;
    if (max_delta < 0.0f) max_delta = 0.0f;
    if (lsq <= max_delta * max_delta || lsq == 0.0f) return target;
    float inv = max_delta / sqrtf(lsq);
    jce_vec3 r = { cur.x + d.x * inv, cur.y + d.y * inv, cur.z + d.z * inv };
    return r;
}

jce_vec3 jce_v3_smooth_damp(jce_vec3 cur, jce_vec3 target, jce_vec3 *vel,
                             float smooth_time, float max_speed, float dt)
{
    if (!vel) return target;
    jce_vec3 out;
    out.x = jce_smooth_damp(cur.x, target.x, &vel->x, smooth_time, max_speed, dt);
    out.y = jce_smooth_damp(cur.y, target.y, &vel->y, smooth_time, max_speed, dt);
    out.z = jce_smooth_damp(cur.z, target.z, &vel->z, smooth_time, max_speed, dt);
    return out;
}

/* ── Random ──────────────────────────────────────────────────── */

static uint64_t s_rand_state = 0;

static uint64_t lcg_next(void)
{
    if (s_rand_state == 0) {
        s_rand_state = (uint64_t)jce_time_perf_counter();
        if (s_rand_state == 0) s_rand_state = 0xCAFEBABEu;
    }
    /* Knuth MMIX. */
    s_rand_state = s_rand_state * 6364136223846793005ull + 1442695040888963407ull;
    return s_rand_state;
}

void jce_random_set_seed(uint64_t seed)
{
    if (seed == 0) seed = 1;
    s_rand_state = seed;
}

float jce_random_value(void)
{
    /* Take the high 24 bits → [0,1) with 24-bit mantissa. */
    uint64_t x = lcg_next();
    uint32_t hi = (uint32_t)(x >> 40);  /* 24 bits */
    return (float)hi / 16777216.0f;
}

float jce_random_range_f(float lo, float hi)
{
    return lo + (hi - lo) * jce_random_value();
}

int jce_random_range_i(int lo, int hi)
{
    if (hi <= lo) return lo;
    uint64_t x = lcg_next();
    return lo + (int)(x % (uint64_t)(hi - lo));
}

jce_vec3 jce_random_inside_unit_sphere(void)
{
    /* Rejection sampling — uniformly distributed inside unit sphere. */
    for (int i = 0; i < 64; ++i) {
        float x = jce_random_range_f(-1.0f, 1.0f);
        float y = jce_random_range_f(-1.0f, 1.0f);
        float z = jce_random_range_f(-1.0f, 1.0f);
        if (x*x + y*y + z*z <= 1.0f) {
            jce_vec3 r = { x, y, z };
            return r;
        }
    }
    /* Practically unreachable. */
    jce_vec3 r = { 0.0f, 0.0f, 0.0f };
    return r;
}

jce_vec3 jce_random_on_unit_sphere(void)
{
    /* Box-Muller style: pick a direction by sampling spherical
     * coordinates uniformly. */
    float theta = jce_random_range_f(0.0f, 2.0f * JCE_PI);
    float u     = jce_random_range_f(-1.0f, 1.0f);
    float s     = sqrtf(1.0f - u * u);
    jce_vec3 r = { s * cosf(theta), s * sinf(theta), u };
    return r;
}

/* ── AABB ────────────────────────────────────────────────────── */

JceAABB jce_aabb_from_center_extents(jce_vec3 c, jce_vec3 he)
{
    JceAABB a;
    a.min.x = c.x - he.x;  a.max.x = c.x + he.x;
    a.min.y = c.y - he.y;  a.max.y = c.y + he.y;
    a.min.z = c.z - he.z;  a.max.z = c.z + he.z;
    return a;
}

JceAABB jce_aabb_empty(void)
{
    JceAABB a = { { 1e30f,  1e30f,  1e30f},
                   {-1e30f, -1e30f, -1e30f} };
    return a;
}

bool jce_aabb_is_empty(JceAABB a)
{
    return a.min.x > a.max.x || a.min.y > a.max.y || a.min.z > a.max.z;
}

JceAABB jce_aabb_encapsulate(JceAABB a, jce_vec3 p)
{
    if (p.x < a.min.x) a.min.x = p.x;
    if (p.y < a.min.y) a.min.y = p.y;
    if (p.z < a.min.z) a.min.z = p.z;
    if (p.x > a.max.x) a.max.x = p.x;
    if (p.y > a.max.y) a.max.y = p.y;
    if (p.z > a.max.z) a.max.z = p.z;
    return a;
}

JceAABB jce_aabb_union(JceAABB a, JceAABB b)
{
    JceAABB r;
    r.min.x = a.min.x < b.min.x ? a.min.x : b.min.x;
    r.min.y = a.min.y < b.min.y ? a.min.y : b.min.y;
    r.min.z = a.min.z < b.min.z ? a.min.z : b.min.z;
    r.max.x = a.max.x > b.max.x ? a.max.x : b.max.x;
    r.max.y = a.max.y > b.max.y ? a.max.y : b.max.y;
    r.max.z = a.max.z > b.max.z ? a.max.z : b.max.z;
    return r;
}

bool jce_aabb_contains(JceAABB a, jce_vec3 p)
{
    return p.x >= a.min.x && p.x <= a.max.x
        && p.y >= a.min.y && p.y <= a.max.y
        && p.z >= a.min.z && p.z <= a.max.z;
}

bool jce_aabb_intersects(JceAABB a, JceAABB b)
{
    return !(a.max.x < b.min.x || a.min.x > b.max.x
          || a.max.y < b.min.y || a.min.y > b.max.y
          || a.max.z < b.min.z || a.min.z > b.max.z);
}

jce_vec3 jce_aabb_center(JceAABB a)
{
    jce_vec3 c = { (a.min.x + a.max.x) * 0.5f,
                    (a.min.y + a.max.y) * 0.5f,
                    (a.min.z + a.max.z) * 0.5f };
    return c;
}

jce_vec3 jce_aabb_extents(JceAABB a)
{
    jce_vec3 e = { (a.max.x - a.min.x) * 0.5f,
                    (a.max.y - a.min.y) * 0.5f,
                    (a.max.z - a.min.z) * 0.5f };
    return e;
}
