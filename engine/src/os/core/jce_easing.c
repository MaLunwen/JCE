/*
 * jce_easing.c -- Standard Penner easing functions (see jce_easing.h).
 */

#include <jce/os/core/jce_easing.h>

#include <math.h>

#define EASE_PI 3.14159265358979323846f

static float clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

static float ease_bounce_out(float t)
{
    const float n1 = 7.5625f, d1 = 2.75f;
    if (t < 1.0f / d1)        return n1 * t * t;
    if (t < 2.0f / d1)      { t -= 1.5f  / d1; return n1 * t * t + 0.75f; }
    if (t < 2.5f / d1)      { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
    t -= 2.625f / d1;       return n1 * t * t + 0.984375f;
}

float jce_ease(JceEaseType type, float t)
{
    t = clamp01(t);
    switch (type) {
        case JCE_EASE_LINEAR:      return t;

        case JCE_EASE_QUAD_IN:     return t * t;
        case JCE_EASE_QUAD_OUT:    return 1.0f - (1.0f - t) * (1.0f - t);
        case JCE_EASE_QUAD_INOUT:  return t < 0.5f ? 2.0f * t * t
                                                   : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) / 2.0f;

        case JCE_EASE_CUBIC_IN:    return t * t * t;
        case JCE_EASE_CUBIC_OUT:   return 1.0f - powf(1.0f - t, 3.0f);
        case JCE_EASE_CUBIC_INOUT: return t < 0.5f ? 4.0f * t * t * t
                                                   : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) / 2.0f;

        case JCE_EASE_QUART_IN:    return t * t * t * t;
        case JCE_EASE_QUART_OUT:   return 1.0f - powf(1.0f - t, 4.0f);
        case JCE_EASE_QUART_INOUT: return t < 0.5f ? 8.0f * t * t * t * t
                                                   : 1.0f - powf(-2.0f * t + 2.0f, 4.0f) / 2.0f;

        case JCE_EASE_QUINT_IN:    return t * t * t * t * t;
        case JCE_EASE_QUINT_OUT:   return 1.0f - powf(1.0f - t, 5.0f);
        case JCE_EASE_QUINT_INOUT: return t < 0.5f ? 16.0f * t * t * t * t * t
                                                   : 1.0f - powf(-2.0f * t + 2.0f, 5.0f) / 2.0f;

        case JCE_EASE_SINE_IN:     return 1.0f - cosf((t * EASE_PI) / 2.0f);
        case JCE_EASE_SINE_OUT:    return sinf((t * EASE_PI) / 2.0f);
        case JCE_EASE_SINE_INOUT:  return -(cosf(EASE_PI * t) - 1.0f) / 2.0f;

        case JCE_EASE_EXPO_IN:     return t <= 0.0f ? 0.0f : powf(2.0f, 10.0f * t - 10.0f);
        case JCE_EASE_EXPO_OUT:    return t >= 1.0f ? 1.0f : 1.0f - powf(2.0f, -10.0f * t);
        case JCE_EASE_EXPO_INOUT:
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return t < 0.5f ? powf(2.0f, 20.0f * t - 10.0f) / 2.0f
                            : (2.0f - powf(2.0f, -20.0f * t + 10.0f)) / 2.0f;

        case JCE_EASE_CIRC_IN:     return 1.0f - sqrtf(1.0f - t * t);
        case JCE_EASE_CIRC_OUT:    return sqrtf(1.0f - (t - 1.0f) * (t - 1.0f));
        case JCE_EASE_CIRC_INOUT:
            return t < 0.5f ? (1.0f - sqrtf(1.0f - powf(2.0f * t, 2.0f))) / 2.0f
                            : (sqrtf(1.0f - powf(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;

        case JCE_EASE_BACK_IN: {
            const float c1 = 1.70158f, c3 = c1 + 1.0f;
            return c3 * t * t * t - c1 * t * t;
        }
        case JCE_EASE_BACK_OUT: {
            const float c1 = 1.70158f, c3 = c1 + 1.0f, u = t - 1.0f;
            return 1.0f + c3 * u * u * u + c1 * u * u;
        }
        case JCE_EASE_BACK_INOUT: {
            const float c1 = 1.70158f, c2 = c1 * 1.525f;
            return t < 0.5f
                ? (powf(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f
                : (powf(2.0f * t - 2.0f, 2.0f) * ((c2 + 1.0f) * (t * 2.0f - 2.0f) + c2) + 2.0f) / 2.0f;
        }

        case JCE_EASE_ELASTIC_IN: {
            const float c4 = (2.0f * EASE_PI) / 3.0f;
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return -powf(2.0f, 10.0f * t - 10.0f) * sinf((t * 10.0f - 10.75f) * c4);
        }
        case JCE_EASE_ELASTIC_OUT: {
            const float c4 = (2.0f * EASE_PI) / 3.0f;
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
        }
        case JCE_EASE_ELASTIC_INOUT: {
            const float c5 = (2.0f * EASE_PI) / 4.5f;
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return t < 0.5f
                ? -(powf(2.0f, 20.0f * t - 10.0f) * sinf((20.0f * t - 11.125f) * c5)) / 2.0f
                :  (powf(2.0f, -20.0f * t + 10.0f) * sinf((20.0f * t - 11.125f) * c5)) / 2.0f + 1.0f;
        }

        case JCE_EASE_BOUNCE_IN:    return 1.0f - ease_bounce_out(1.0f - t);
        case JCE_EASE_BOUNCE_OUT:   return ease_bounce_out(t);
        case JCE_EASE_BOUNCE_INOUT:
            return t < 0.5f ? (1.0f - ease_bounce_out(1.0f - 2.0f * t)) / 2.0f
                            : (1.0f + ease_bounce_out(2.0f * t - 1.0f)) / 2.0f;

        default: return t;
    }
}

float jce_ease_lerp(JceEaseType type, float a, float b, float t)
{
    return a + (b - a) * jce_ease(type, t);
}

const char *jce_ease_name(JceEaseType type)
{
    static const char *kNames[JCE_EASE_COUNT] = {
        "Linear",
        "QuadIn", "QuadOut", "QuadInOut",
        "CubicIn", "CubicOut", "CubicInOut",
        "QuartIn", "QuartOut", "QuartInOut",
        "QuintIn", "QuintOut", "QuintInOut",
        "SineIn", "SineOut", "SineInOut",
        "ExpoIn", "ExpoOut", "ExpoInOut",
        "CircIn", "CircOut", "CircInOut",
        "BackIn", "BackOut", "BackInOut",
        "ElasticIn", "ElasticOut", "ElasticInOut",
        "BounceIn", "BounceOut", "BounceInOut",
    };
    if (type < 0 || type >= JCE_EASE_COUNT) return "Linear";
    return kNames[type];
}
