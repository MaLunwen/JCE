/*
 * jce_tween.h  Procedural property tweens (DOTween-style).
 *
 * A tween animates a writable target field from a start value to an
 * end value over `duration` seconds with an easing curve.  Useful for
 * UI fades, camera shakes, doors swinging, score-popup pop, etc. —
 * all the property-anim cases that shouldn't need an authored clip.
 *
 * Design:
 *   - Supports float / vec3 / color-RGBA targets.  Vector and color
 *     are ELEMENT-WISE eased so each component animates in lock-step.
 *   - Pool-backed (no allocs after the first N tweens).  Calls to
 *     `jce_tweens_tick(dt)` advance every live tween and write the
 *     interpolated value back through the user-supplied target ptr.
 *   - One easing curve per tween from a small set (linear, in/out,
 *     quad, cubic, sine, back, bounce).  Custom curves can be plugged
 *     via `JceAnimCurve` (Batch 7) for richer control.
 *
 * Layer: middleware / animation (Layer 3) — public.
 */

#ifndef JCE_TWEEN_H
#define JCE_TWEEN_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_EASE_LINEAR = 0,
    JCE_EASE_IN_QUAD,
    JCE_EASE_OUT_QUAD,
    JCE_EASE_IN_OUT_QUAD,
    JCE_EASE_IN_CUBIC,
    JCE_EASE_OUT_CUBIC,
    JCE_EASE_IN_OUT_CUBIC,
    JCE_EASE_IN_SINE,
    JCE_EASE_OUT_SINE,
    JCE_EASE_IN_OUT_SINE,
    JCE_EASE_OUT_BACK,
    JCE_EASE_OUT_BOUNCE,
    JCE_EASE_COUNT
} JceEase;

typedef uint32_t JceTweenId;
#define JCE_TWEEN_INVALID 0u

typedef enum {
    JCE_TWEEN_TARGET_FLOAT = 0,
    JCE_TWEEN_TARGET_VEC3  = 1,
    JCE_TWEEN_TARGET_VEC4  = 2,    /* used for RGBA color too */
} JceTweenTargetKind;

typedef void (*JceTweenCompleteFn)(JceTweenId id, void *user_data);

typedef struct {
    JceTweenTargetKind kind;
    void              *target;        /* float* / jce_vec3* / jce_vec4* */
    /* Start/end (only the first `kind`-many components are read). */
    float              start[4];
    float              end[4];
    float              duration;      /* seconds */
    float              delay;         /* seconds before first sample */
    JceEase            ease;
    bool               loop;          /* restart on completion */
    bool               yoyo;          /* alternate direction each loop */
    JceTweenCompleteFn on_complete;
    void              *user_data;
} JceTweenDesc;

JCE_API JceTweenId jce_tween_start(const JceTweenDesc *desc);
JCE_API void       jce_tween_kill (JceTweenId id);
JCE_API bool       jce_tween_is_alive(JceTweenId id);

JCE_API uint32_t   jce_tweens_alive_count(void);

/* Advance all live tweens by `dt` seconds.  Call once per frame. */
JCE_API void       jce_tweens_tick(float dt);

/* Convenience builders — typical Unity code pattern. */
JCE_API JceTweenId jce_tween_float(float *target, float to, float duration,
                                    JceEase ease);
JCE_API JceTweenId jce_tween_vec3 (jce_vec3 *target, jce_vec3 to,
                                    float duration, JceEase ease);
JCE_API JceTweenId jce_tween_color(jce_vec4 *target, jce_vec4 to,
                                    float duration, JceEase ease);

/* Evaluate an easing curve directly at t ∈ [0,1].  Useful for
 * gameplay code that wants the curve without spawning a tween. */
JCE_API float      jce_ease_eval(JceEase e, float t);

JCE_EXTERN_C_END

#endif /* JCE_TWEEN_H */
