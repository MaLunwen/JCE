/*
 * jce_vcam_blend.c  Blend curves between two virtual cameras.
 */

#include <jce/middleware/scene/jce_vcam_blend.h>

#include <string.h>

void jce_vcam_blend_init(JceVCamBlendState *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
}

void jce_vcam_blend_start(JceVCamBlendState *b, uint64_t from, uint64_t to,
                            JceVCamBlendCurve curve, float dur)
{
    if (!b) return;
    b->from_vcam = from;
    b->to_vcam   = to;
    b->spec.curve = curve;
    b->spec.duration_seconds = dur > 0 ? dur : 0.0f;
    b->elapsed = 0.0f;
    b->active  = true;
    if (curve == JCE_VCAM_BLEND_CUT || dur <= 0.0f) {
        b->elapsed = b->spec.duration_seconds; /* instantly complete */
    }
}

void jce_vcam_blend_tick(JceVCamBlendState *b, float dt)
{
    if (!b || !b->active) return;
    b->elapsed += dt;
    if (b->elapsed >= b->spec.duration_seconds) {
        b->elapsed = b->spec.duration_seconds;
        /* Keep `active` true so consumer reads the final t=1.0 once,
         * then explicitly call init() / start() again. */
    }
}

static float apply_curve(JceVCamBlendCurve c, float t)
{
    if (t < 0) t = 0; else if (t > 1) t = 1;
    switch (c) {
    case JCE_VCAM_BLEND_LINEAR:    return t;
    case JCE_VCAM_BLEND_EASE_IN:   return t * t;
    case JCE_VCAM_BLEND_EASE_OUT:  return 1.0f - (1.0f - t) * (1.0f - t);
    case JCE_VCAM_BLEND_EASE_INOUT:
        return (t < 0.5f) ? 2.0f * t * t
                          : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    case JCE_VCAM_BLEND_CUT:
    default:
        return 1.0f;
    }
}

float jce_vcam_blend_evaluate(const JceVCamBlendState *b)
{
    if (!b || !b->active || b->spec.duration_seconds <= 0.0f) return 1.0f;
    float raw = b->elapsed / b->spec.duration_seconds;
    return apply_curve(b->spec.curve, raw);
}
