/*
 * jce_math_ext.h  Unity-style math conveniences.
 *
 * Wraps the existing jce_math.h primitives with the high-level
 * helpers gameplay code uses every day:
 *   - clamp / saturate / repeat / pingpong / smoothstep / remap
 *   - move_towards (constant-rate approach)
 *   - smooth_damp (critically-damped spring; same curve as Unity's
 *     Mathf.SmoothDamp / Vector3.SmoothDamp)
 *   - axis-aligned bounding box (AABB) helpers
 *
 * All pure functions, no allocations.  Layer: os/core (Layer 1).
 */

#ifndef JCE_MATH_EXT_H
#define JCE_MATH_EXT_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* ── Scalar helpers ──────────────────────────────────────────── */

JCE_API float jce_clamp     (float v, float lo, float hi);
JCE_API float jce_clamp01   (float v);
JCE_API float jce_repeat    (float t, float length);          /* fmod that wraps neg */
JCE_API float jce_pingpong  (float t, float length);
JCE_API float jce_smoothstep(float edge0, float edge1, float t);
JCE_API float jce_remap     (float v, float in_min, float in_max,
                              float out_min, float out_max);
JCE_API float jce_lerp_unclamped(float a, float b, float t);
JCE_API float jce_inverse_lerp  (float a, float b, float v);  /* (v-a)/(b-a) clamped */

/* Approach `target` at constant rate `max_delta`.  Returns the new
 * value.  Stops exactly at target so callers can test equality. */
JCE_API float jce_move_towards(float current, float target, float max_delta);

/* Critically-damped spring.  `velocity` is read+written (so the
 * caller can persist it across frames).  `smooth_time` ≈ time to
 * reach 63.2% of target; smaller = snappier.  `max_speed` caps the
 * approach velocity. */
JCE_API float jce_smooth_damp(float  current,
                              float  target,
                              float *velocity,
                              float  smooth_time,
                              float  max_speed,
                              float  dt);

/* ── Vector helpers ──────────────────────────────────────────── */

JCE_API jce_vec3 jce_v3_move_towards(jce_vec3 current, jce_vec3 target,
                                      float max_delta);

JCE_API jce_vec3 jce_v3_smooth_damp(jce_vec3 current, jce_vec3 target,
                                     jce_vec3 *velocity,
                                     float    smooth_time,
                                     float    max_speed,
                                     float    dt);

/* ── Random helpers (Unity's UnityEngine.Random) ─────────────── *
 *
 * Single-stream LCG.  Seeded automatically from time on first use;
 * caller can override with set_seed for reproducibility (tests,
 * replay determinism). */

JCE_API void  jce_random_set_seed(uint64_t seed);
JCE_API float jce_random_value(void);                /* [0, 1) */
JCE_API float jce_random_range_f(float lo, float hi);/* [lo, hi) */
JCE_API int   jce_random_range_i(int lo, int hi);    /* [lo, hi) */
JCE_API jce_vec3 jce_random_inside_unit_sphere(void);
JCE_API jce_vec3 jce_random_on_unit_sphere(void);

/* ── AABB ────────────────────────────────────────────────────── */

typedef struct {
    jce_vec3 min;
    jce_vec3 max;
} JceAABB;

JCE_API JceAABB jce_aabb_from_center_extents(jce_vec3 center, jce_vec3 half);
JCE_API JceAABB jce_aabb_empty(void);                 /* center 0, sentinel min>max */
JCE_API bool    jce_aabb_is_empty(JceAABB a);
JCE_API JceAABB jce_aabb_encapsulate(JceAABB a, jce_vec3 p);
JCE_API JceAABB jce_aabb_union(JceAABB a, JceAABB b);
JCE_API bool    jce_aabb_contains(JceAABB a, jce_vec3 p);
JCE_API bool    jce_aabb_intersects(JceAABB a, JceAABB b);
JCE_API jce_vec3 jce_aabb_center(JceAABB a);
JCE_API jce_vec3 jce_aabb_extents(JceAABB a);          /* half-size */

JCE_EXTERN_C_END

#endif /* JCE_MATH_EXT_H */
