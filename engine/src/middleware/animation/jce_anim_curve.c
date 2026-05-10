/*
 * jce_anim_curve.c  Cubic-Hermite scalar curve.
 *
 * Hermite formula for segment between (t0, v0, m0) and (t1, v1, m1):
 *   h = t1 - t0
 *   u = (t - t0) / h
 *   value = (2u³ - 3u² + 1) v0
 *         + (u³ - 2u² + u) m0 h
 *         + (-2u³ + 3u²)    v1
 *         + (u³ - u²)       m1 h
 *
 * The keyframes' `out_tangent` of key i and `in_tangent` of key i+1
 * play the role of m0 / m1 respectively — matching Unity's authoring
 * convention.
 */

#include <jce/middleware/animation/jce_anim_curve.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

struct JceAnimCurve {
    JceCurveKey      *keys;
    uint32_t          count;
    uint32_t          capacity;
    JceCurveWrapMode  pre_wrap;
    JceCurveWrapMode  post_wrap;
};

JceAnimCurve *jce_anim_curve_create(uint32_t initial_capacity)
{
    JceAnimCurve *c = (JceAnimCurve *)JCE_CALLOC(1, sizeof(*c));
    if (!c) return NULL;
    c->capacity = initial_capacity ? initial_capacity : 8u;
    c->keys = (JceCurveKey *)JCE_CALLOC(c->capacity, sizeof(JceCurveKey));
    if (!c->keys) { JCE_FREE(c); return NULL; }
    return c;
}

void jce_anim_curve_destroy(JceAnimCurve *c)
{
    if (!c) return;
    JCE_FREE(c->keys);
    JCE_FREE(c);
}

void jce_anim_curve_set_wrap(JceAnimCurve *c,
                              JceCurveWrapMode pre, JceCurveWrapMode post)
{
    if (!c) return;
    c->pre_wrap  = pre;
    c->post_wrap = post;
}

JceCurveWrapMode jce_anim_curve_pre_wrap (const JceAnimCurve *c)
{ return c ? c->pre_wrap  : JCE_CURVE_WRAP_CLAMP; }
JceCurveWrapMode jce_anim_curve_post_wrap(const JceAnimCurve *c)
{ return c ? c->post_wrap : JCE_CURVE_WRAP_CLAMP; }

uint32_t jce_anim_curve_count(const JceAnimCurve *c)
{ return c ? c->count : 0u; }

const JceCurveKey *jce_anim_curve_at(const JceAnimCurve *c, uint32_t idx)
{
    if (!c || idx >= c->count) return NULL;
    return &c->keys[idx];
}

void jce_anim_curve_clear(JceAnimCurve *c)
{
    if (!c) return;
    c->count = 0;
}

static bool ensure_cap(JceAnimCurve *c, uint32_t need)
{
    if (c->capacity >= need) return true;
    uint32_t cap = c->capacity ? c->capacity * 2u : 8u;
    while (cap < need) cap *= 2u;
    JceCurveKey *p = (JceCurveKey *)JCE_REALLOC(c->keys,
                                                 cap * sizeof(JceCurveKey));
    if (!p) return false;
    c->keys = p;
    c->capacity = cap;
    return true;
}

uint32_t jce_anim_curve_add_key(JceAnimCurve *c, JceCurveKey k)
{
    if (!c) return UINT32_MAX;
    /* Find insertion / update index. */
    uint32_t i = 0;
    while (i < c->count && c->keys[i].time < k.time) i++;
    if (i < c->count && c->keys[i].time == k.time) {
        c->keys[i] = k;
        return i;
    }
    if (!ensure_cap(c, c->count + 1)) return UINT32_MAX;
    /* Shift right. */
    if (i < c->count)
        memmove(&c->keys[i + 1], &c->keys[i],
                (c->count - i) * sizeof(JceCurveKey));
    c->keys[i] = k;
    c->count++;
    return i;
}

bool jce_anim_curve_remove_at(JceAnimCurve *c, uint32_t idx)
{
    if (!c || idx >= c->count) return false;
    if (idx + 1 < c->count)
        memmove(&c->keys[idx], &c->keys[idx + 1],
                (c->count - idx - 1) * sizeof(JceCurveKey));
    c->count--;
    return true;
}

/* ── Evaluate ────────────────────────────────────────────────────── */

static float wrap_time(float t, float t0, float t1, JceCurveWrapMode mode)
{
    float span = t1 - t0;
    if (span <= 0.0f) return t0;
    if (mode == JCE_CURVE_WRAP_CLAMP) {
        if (t < t0) return t0;
        if (t > t1) return t1;
        return t;
    }
    /* Bring `t` into [t0, t1] under the chosen wrap mode. */
    float dt = t - t0;
    float n  = floorf(dt / span);
    float r  = dt - n * span;
    if (mode == JCE_CURVE_WRAP_REPEAT) {
        return t0 + r;
    }
    /* Pingpong: even n → forward, odd n → reflected. */
    int even = (((int)n) & 1) == 0;
    return even ? (t0 + r) : (t1 - r);
}

float jce_anim_curve_evaluate(const JceAnimCurve *c, float t)
{
    if (!c || c->count == 0) return 0.0f;
    if (c->count == 1)       return c->keys[0].value;

    float t0 = c->keys[0].time;
    float t1 = c->keys[c->count - 1].time;
    if (t < t0) t = wrap_time(t, t0, t1, c->pre_wrap);
    if (t > t1) t = wrap_time(t, t0, t1, c->post_wrap);

    /* Binary search the segment whose left key is the largest with
     * time <= t. */
    uint32_t lo = 0, hi = c->count - 1;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (c->keys[mid].time <= t) lo = mid; else hi = mid;
    }
    const JceCurveKey *a = &c->keys[lo];
    const JceCurveKey *b = &c->keys[lo + 1];
    float h = b->time - a->time;
    if (h <= 0.0f) return a->value;
    float u = (t - a->time) / h;
    float u2 = u * u;
    float u3 = u2 * u;

    float h00 =  2.0f*u3 - 3.0f*u2 + 1.0f;
    float h10 =        u3 - 2.0f*u2 + u;
    float h01 = -2.0f*u3 + 3.0f*u2;
    float h11 =        u3 -        u2;
    return h00 * a->value
         + h10 * (a->out_tangent * h)
         + h01 * b->value
         + h11 * (b->in_tangent  * h);
}

JceAnimCurve *jce_anim_curve_from_linear(float t0, float v0, float t1, float v1)
{
    JceAnimCurve *c = jce_anim_curve_create(2);
    if (!c) return NULL;
    /* Linear curve = matched tangents derived from rise/run. */
    float slope = (t1 > t0) ? (v1 - v0) / (t1 - t0) : 0.0f;
    JceCurveKey k0 = { t0, v0, slope, slope };
    JceCurveKey k1 = { t1, v1, slope, slope };
    jce_anim_curve_add_key(c, k0);
    jce_anim_curve_add_key(c, k1);
    return c;
}
