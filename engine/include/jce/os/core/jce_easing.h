/*
 * jce_easing.h -- Standard easing (tween) functions.
 *
 * The classic Penner easing set, normalised: every easing maps t in [0,1] to a
 * value (mostly in [0,1], with Back/Elastic intentionally overshooting).  Used
 * by the sequencer (keyframe interpolation), UI animations, and camera blends.
 * Pure math, dependency-free, fully unit-testable.
 *
 * Layer: OS / Core (Layer 1).
 */

#ifndef JCE_EASING_H
#define JCE_EASING_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_EASE_LINEAR = 0,
    JCE_EASE_QUAD_IN,    JCE_EASE_QUAD_OUT,    JCE_EASE_QUAD_INOUT,
    JCE_EASE_CUBIC_IN,   JCE_EASE_CUBIC_OUT,   JCE_EASE_CUBIC_INOUT,
    JCE_EASE_QUART_IN,   JCE_EASE_QUART_OUT,   JCE_EASE_QUART_INOUT,
    JCE_EASE_QUINT_IN,   JCE_EASE_QUINT_OUT,   JCE_EASE_QUINT_INOUT,
    JCE_EASE_SINE_IN,    JCE_EASE_SINE_OUT,    JCE_EASE_SINE_INOUT,
    JCE_EASE_EXPO_IN,    JCE_EASE_EXPO_OUT,    JCE_EASE_EXPO_INOUT,
    JCE_EASE_CIRC_IN,    JCE_EASE_CIRC_OUT,    JCE_EASE_CIRC_INOUT,
    JCE_EASE_BACK_IN,    JCE_EASE_BACK_OUT,    JCE_EASE_BACK_INOUT,
    JCE_EASE_ELASTIC_IN, JCE_EASE_ELASTIC_OUT, JCE_EASE_ELASTIC_INOUT,
    JCE_EASE_BOUNCE_IN,  JCE_EASE_BOUNCE_OUT,  JCE_EASE_BOUNCE_INOUT,
    JCE_EASE_COUNT
} JceEaseType;

/* Evaluate an easing at `t` (clamped to [0,1]).  Unknown type → linear. */
JCE_API float jce_ease(JceEaseType type, float t);

/* Eased interpolation: a + (b - a) * jce_ease(type, t). */
JCE_API float jce_ease_lerp(JceEaseType type, float a, float b, float t);

/* Stable identifier string (for editor dropdowns / serialization). */
JCE_API const char *jce_ease_name(JceEaseType type);

JCE_EXTERN_C_END

#endif /* JCE_EASING_H */
