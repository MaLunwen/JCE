/*
 * jce_anim_curve.h  Keyframe-driven scalar animation curves.
 *
 * Mirrors Unity's `AnimationCurve`: an ordered list of keyframes
 * (time, value, in_tangent, out_tangent) sampled with cubic Hermite
 * interpolation between adjacent keys.  Useful for any scalar that
 * varies over time: emission rates, post-FX intensity, camera lens
 * parameters, audio volume curves, character speed curves, etc.
 *
 * The curve owns its keyframe array and supports edit-time mutation
 * (insert / remove / move) plus runtime sampling.  Wrap modes control
 * what happens outside the [first.time, last.time] range.
 *
 * Layer: middleware / animation (Layer 3) — public.
 */

#ifndef JCE_ANIM_CURVE_H
#define JCE_ANIM_CURVE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_CURVE_WRAP_CLAMP    = 0,
    JCE_CURVE_WRAP_REPEAT   = 1,
    JCE_CURVE_WRAP_PINGPONG = 2
} JceCurveWrapMode;

typedef struct {
    float time;
    float value;
    float in_tangent;   /* slope at the key on the left side */
    float out_tangent;  /* slope at the key on the right side */
} JceCurveKey;

typedef struct JceAnimCurve JceAnimCurve;

/* Lifecycle.  initial_capacity 0 → default (8). */
JCE_API JceAnimCurve *jce_anim_curve_create(uint32_t initial_capacity);
JCE_API void          jce_anim_curve_destroy(JceAnimCurve *c);

JCE_API void              jce_anim_curve_set_wrap(JceAnimCurve *c,
                                                  JceCurveWrapMode pre,
                                                  JceCurveWrapMode post);
JCE_API JceCurveWrapMode  jce_anim_curve_pre_wrap (const JceAnimCurve *c);
JCE_API JceCurveWrapMode  jce_anim_curve_post_wrap(const JceAnimCurve *c);

/* Insert a key, keeping the keys sorted by time.  Replaces an
 * existing key whose time matches exactly.  Returns the index of the
 * inserted/updated key, or UINT32_MAX on OOM. */
JCE_API uint32_t jce_anim_curve_add_key(JceAnimCurve *c, JceCurveKey k);

/* Remove a key by index.  Returns true if removed. */
JCE_API bool jce_anim_curve_remove_at(JceAnimCurve *c, uint32_t idx);

/* Reset to empty (capacity preserved). */
JCE_API void jce_anim_curve_clear(JceAnimCurve *c);

JCE_API uint32_t           jce_anim_curve_count(const JceAnimCurve *c);
JCE_API const JceCurveKey *jce_anim_curve_at   (const JceAnimCurve *c, uint32_t idx);

/* Sample at time t.  Empty curves return 0; single-key curves return
 * that key's value.  Otherwise cubic-Hermite-interpolates between the
 * two surrounding keys.  Out-of-range times honour the wrap mode. */
JCE_API float jce_anim_curve_evaluate(const JceAnimCurve *c, float t);

/* Convenience: build a linear curve from two end-points (no
 * tangent authoring).  Useful for rapid setup + tests. */
JCE_API JceAnimCurve *jce_anim_curve_from_linear(float t0, float v0,
                                                  float t1, float v1);

JCE_EXTERN_C_END

#endif /* JCE_ANIM_CURVE_H */
