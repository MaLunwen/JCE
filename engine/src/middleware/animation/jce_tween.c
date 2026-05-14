/*
 * jce_tween.c  Procedural property tween runtime.
 *
 * Pool of 256 tween slots.  Each tick: advance time, evaluate ease,
 * write interpolated value back through target ptr.  Loop / yoyo
 * handled by reflecting `t` after `duration` is reached.
 */

#include <jce/middleware/animation/jce_tween.h>

#include <math.h>
#include <string.h>

#define TWEEN_MAX 256

#ifndef JCE_PI
#define JCE_PI 3.14159265358979323846f
#endif

typedef struct {
    bool          alive;
    JceTweenDesc  desc;
    float         elapsed;
    float         dir;          /* +1 forward, -1 reverse (yoyo) */
} Slot;

static Slot     s_slots[TWEEN_MAX];
static uint32_t s_id_counter = 1u;
static uint32_t s_id_for_slot[TWEEN_MAX];

/* ── Easing curves ──────────────────────────────────────────────── */

static float ease_in_quad   (float t) { return t * t; }
static float ease_out_quad  (float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
static float ease_in_out_quad(float t)
{ return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) * 0.5f; }
static float ease_in_cubic  (float t) { return t * t * t; }
static float ease_out_cubic (float t) { float u = 1.0f - t; return 1.0f - u*u*u; }
static float ease_in_out_cubic(float t)
{ return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f; }
static float ease_in_sine   (float t) { return 1.0f - cosf((t * JCE_PI) * 0.5f); }
static float ease_out_sine  (float t) { return sinf((t * JCE_PI) * 0.5f); }
static float ease_in_out_sine(float t) { return -(cosf(JCE_PI * t) - 1.0f) * 0.5f; }
static float ease_out_back  (float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * powf(t - 1.0f, 3.0f) + c1 * powf(t - 1.0f, 2.0f);
}
static float ease_out_bounce(float t)
{
    const float n1 = 7.5625f;
    const float d1 = 2.75f;
    if (t < 1.0f / d1)             return n1 * t * t;
    else if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
    else if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    else                    { t -= 2.625f / d1; return n1 * t * t + 0.984375f; }
}

float jce_ease_eval(JceEase e, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    switch (e) {
        case JCE_EASE_LINEAR:        return t;
        case JCE_EASE_IN_QUAD:       return ease_in_quad(t);
        case JCE_EASE_OUT_QUAD:      return ease_out_quad(t);
        case JCE_EASE_IN_OUT_QUAD:   return ease_in_out_quad(t);
        case JCE_EASE_IN_CUBIC:      return ease_in_cubic(t);
        case JCE_EASE_OUT_CUBIC:     return ease_out_cubic(t);
        case JCE_EASE_IN_OUT_CUBIC:  return ease_in_out_cubic(t);
        case JCE_EASE_IN_SINE:       return ease_in_sine(t);
        case JCE_EASE_OUT_SINE:      return ease_out_sine(t);
        case JCE_EASE_IN_OUT_SINE:   return ease_in_out_sine(t);
        case JCE_EASE_OUT_BACK:      return ease_out_back(t);
        case JCE_EASE_OUT_BOUNCE:    return ease_out_bounce(t);
        default:                     return t;
    }
}

/* ── Pool ───────────────────────────────────────────────────────── */

static int find_free_slot(void)
{
    for (int i = 0; i < TWEEN_MAX; ++i)
        if (!s_slots[i].alive) return i;
    return -1;
}

static int find_slot_by_id(JceTweenId id)
{
    for (int i = 0; i < TWEEN_MAX; ++i)
        if (s_slots[i].alive && s_id_for_slot[i] == id) return i;
    return -1;
}

JceTweenId jce_tween_start(const JceTweenDesc *d)
{
    if (!d || !d->target || d->duration <= 0.0f) return JCE_TWEEN_INVALID;
    int slot = find_free_slot();
    if (slot < 0) return JCE_TWEEN_INVALID;
    Slot *s = &s_slots[slot];
    s->alive   = true;
    s->desc    = *d;
    s->elapsed = -d->delay;  /* negative until delay expires */
    s->dir     = 1.0f;
    JceTweenId id = s_id_counter++;
    if (s_id_counter == JCE_TWEEN_INVALID) s_id_counter = 1u;
    s_id_for_slot[slot] = id;
    return id;
}

void jce_tween_kill(JceTweenId id)
{
    int slot = find_slot_by_id(id);
    if (slot < 0) return;
    s_slots[slot].alive = false;
    s_id_for_slot[slot] = 0;
}

bool jce_tween_is_alive(JceTweenId id)
{
    return find_slot_by_id(id) >= 0;
}

uint32_t jce_tweens_alive_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < TWEEN_MAX; ++i) if (s_slots[i].alive) n++;
    return n;
}

/* ── Tick ──────────────────────────────────────────────────────── */

static int components_for_kind(JceTweenTargetKind k)
{
    switch (k) {
        case JCE_TWEEN_TARGET_FLOAT: return 1;
        case JCE_TWEEN_TARGET_VEC3:  return 3;
        case JCE_TWEEN_TARGET_VEC4:  return 4;
        default: return 0;
    }
}

static void write_target(Slot *s, float u)
{
    int n = components_for_kind(s->desc.kind);
    float *t = (float *)s->desc.target;
    for (int i = 0; i < n; ++i)
        t[i] = s->desc.start[i] + (s->desc.end[i] - s->desc.start[i]) * u;
}

void jce_tweens_tick(float dt)
{
    if (dt < 0.0f) dt = 0.0f;
    for (int i = 0; i < TWEEN_MAX; ++i) {
        Slot *s = &s_slots[i];
        if (!s->alive) continue;
        s->elapsed += dt * s->dir;

        if (s->elapsed < 0.0f) continue;  /* waiting on delay */

        bool finished = false;
        float t = s->elapsed / s->desc.duration;
        if (s->dir > 0.0f && t >= 1.0f) {
            t = 1.0f;
            finished = true;
        } else if (s->dir < 0.0f && t <= 0.0f) {
            t = 0.0f;
            finished = true;
        }
        float u = jce_ease_eval(s->desc.ease, t);
        if (s->dir < 0.0f) u = 1.0f - jce_ease_eval(s->desc.ease, 1.0f - t);
        write_target(s, u);

        if (finished) {
            if (s->desc.loop) {
                if (s->desc.yoyo) {
                    s->dir = -s->dir;
                } else {
                    s->elapsed = 0.0f;
                }
            } else {
                if (s->desc.on_complete)
                    s->desc.on_complete(s_id_for_slot[i], s->desc.user_data);
                s->alive = false;
                s_id_for_slot[i] = 0;
            }
        }
    }
}

/* ── Convenience builders ───────────────────────────────────────── */

JceTweenId jce_tween_float(float *target, float to, float duration, JceEase ease)
{
    if (!target) return JCE_TWEEN_INVALID;
    JceTweenDesc d = {0};
    d.kind = JCE_TWEEN_TARGET_FLOAT;
    d.target = target;
    d.start[0] = *target;
    d.end[0]   = to;
    d.duration = duration;
    d.ease = ease;
    return jce_tween_start(&d);
}

JceTweenId jce_tween_vec3(jce_vec3 *target, jce_vec3 to, float duration,
                           JceEase ease)
{
    if (!target) return JCE_TWEEN_INVALID;
    JceTweenDesc d = {0};
    d.kind = JCE_TWEEN_TARGET_VEC3;
    d.target = target;
    d.start[0] = target->x; d.start[1] = target->y; d.start[2] = target->z;
    d.end[0]   = to.x;      d.end[1]   = to.y;      d.end[2]   = to.z;
    d.duration = duration;
    d.ease = ease;
    return jce_tween_start(&d);
}

JceTweenId jce_tween_color(jce_vec4 *target, jce_vec4 to, float duration,
                            JceEase ease)
{
    if (!target) return JCE_TWEEN_INVALID;
    JceTweenDesc d = {0};
    d.kind = JCE_TWEEN_TARGET_VEC4;
    d.target = target;
    d.start[0] = target->x; d.start[1] = target->y; d.start[2] = target->z; d.start[3] = target->w;
    d.end[0]   = to.x;      d.end[1]   = to.y;      d.end[2]   = to.z;      d.end[3]   = to.w;
    d.duration = duration;
    d.ease = ease;
    return jce_tween_start(&d);
}
