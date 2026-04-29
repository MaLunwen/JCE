/*
 * jce_shadow_filter.h -- generic shadow-map utility helpers.
 *
 * Pure-CPU math.  No bgfx/GPU dependency.  Used by callers that own
 * their own CSM/spot/point shadow rendering (e.g. jce_csm) to:
 *   - Generate Poisson disk sample sets for PCF / PCSS jitter
 *   - Compute practical-split-scheme cascade distances (Engel/Zhang)
 *   - Snap a cascade orthographic frustum to texel grid (kills shimmer)
 *   - Pack a 3x3 / 5x5 PCF kernel weight table
 *
 * Generic — works for any shadow-mapping pipeline.
 *
 * Thread-safety: pure functions on caller-owned output buffers — fully
 * re-entrant; safe to call from any thread.
 *
 * Example:
 *   float disk[64 * 2];
 *   jce_shadow_filter_poisson_disk(64, 0xC0FFEEu, disk);
 *
 *   float splits[5];
 *   jce_shadow_filter_pssm_splits(0.1f, 1000.0f, 4, 0.65f, splits);
 *
 *   jce_vec3 snapped;
 *   jce_shadow_filter_stabilize_center(sphere_c, sphere_r, light_x, light_y,
 *                                      2048, &snapped);
 *
 *   float weights[7];
 *   jce_shadow_filter_gaussian_1d(3, 1.4f, weights);
 *
 * Layer: renderer (Layer 3) — public.
 */
#ifndef JCE_SHADOW_FILTER_H
#define JCE_SHADOW_FILTER_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  Poisson disk samples
 * ------------------------------------------------------------------ */

/* Generate `count` 2D samples in the unit disk via Mitchell's best-
 * candidate algorithm.  Output `out_xy` must hold count*2 floats.
 * `seed` lets the caller make it deterministic across runs.
 */
JCE_API void jce_shadow_filter_poisson_disk(uint32_t count,
                                            uint32_t seed,
                                            float   *out_xy);

/* ------------------------------------------------------------------ *
 *  Cascade split (Practical Split Scheme, Zhang 2006)
 * ------------------------------------------------------------------ */

/* Fill `out_splits[0..cascade_count]` with view-space distances:
 *   out_splits[0] = near, out_splits[cascade_count] = far,
 *   inner splits are PSSM(lambda) blends of log + uniform schemes.
 * lambda in [0,1]: 0 = uniform, 1 = logarithmic.  Typical 0.5-0.75.
 */
JCE_API void jce_shadow_filter_pssm_splits(float    near_plane,
                                           float    far_plane,
                                           uint32_t cascade_count,
                                           float    lambda,
                                           float   *out_splits);

/* ------------------------------------------------------------------ *
 *  Cascade stabilization
 * ------------------------------------------------------------------ */

/* Given a sphere bounding the cascade frustum corners (center, radius)
 * and the shadow-map resolution, compute a snapped light-space center
 * such that the orthographic frustum moves in whole-texel increments
 * along the light's right/up axes.  Eliminates "shadow swimming".
 *
 * `light_right` / `light_up` are unit vectors in world space.
 * `out_snapped_center` is written.
 */
JCE_API void jce_shadow_filter_stabilize_center(jce_vec3        sphere_center,
                                                float         sphere_radius,
                                                jce_vec3        light_right,
                                                jce_vec3        light_up,
                                                uint32_t      shadow_map_size,
                                                jce_vec3       *out_snapped_center);

/* ------------------------------------------------------------------ *
 *  Separable PCF kernel weights
 * ------------------------------------------------------------------ */

/* Build a normalized 1D Gaussian kernel of the given radius.
 * `radius` must be in [1, 8].  Output writes (2*radius+1) floats.
 */
JCE_API void jce_shadow_filter_gaussian_1d(uint32_t radius,
                                           float    sigma,
                                           float   *out_weights);

/* ============================================================ */
/* Variance Shadow Maps (VSM) helpers                            */
/* ============================================================ */
/* CPU-side companions to a future fs_shadow_vsm fragment shader.
 * The shader writes (depth, depth*depth) to an RG16F/RG32F target;
 * sampling reads the moments back and applies Chebyshev's inequality
 * to compute an upper bound on the lit-fraction, which gives soft
 * shadow edges from a single texture tap (no PCF kernel).
 *
 * These helpers exist as the C99 reference for that math so:
 *   - the GPU shader can be cross-checked against a known-good CPU pass
 *   - tests / unit tools can compute expected occlusion on the CPU
 *   - the VSM constants stay in one place
 */

/* Chebyshev's inequality: returns the lit-fraction upper bound for a
 * fragment at depth `t` given the sampled moments (E[depth], E[depth^2]).
 * Returns 1.0 when t <= moments.x (fully lit).
 * `min_variance` clamps numerical noise (typical 1e-5).  The result is
 * meant to be fed through a light-bleed reduction step. */
JCE_API float jce_shadow_filter_vsm_chebyshev(float moment_x,
                                              float moment_y,
                                              float t,
                                              float min_variance);

/* Light-bleed reduction: rescales `p` so values below `amount` (in
 * [0,1)) are clamped to 0, the rest linearly remapped to [0,1].
 * Mitigates VSM's classic over-soft halo around occluders.  Typical
 * `amount` is 0.1–0.3. */
JCE_API float jce_shadow_filter_vsm_reduce_bleed(float p, float amount);

/* ------------------------------------------------------------------ *
 *  Shadow mode selector
 * ------------------------------------------------------------------ */
/* Renderer-facing enum picking which shadow-mapping technique the
 * pipeline should use for the active CSM cascades.  Defaults to
 * PCF — the legacy path.  VSM requires the `shadow_vsm` shader pair
 * (engine/shaders/pbr/{vs,fs}_shadow_vsm.sc) plus an RG16F/RG32F
 * shadow target; renderers that don't honour the request must fall
 * back to PCF and log a warning. */
typedef enum JceShadowMode {
    JCE_SHADOW_MODE_PCF = 0,  /* Hardware-PCF or software 3x3/5x5    */
    JCE_SHADOW_MODE_VSM = 1,  /* Variance Shadow Maps (Donnelly 2006) */
} JceShadowMode;

/* String form for logging/profiler ("PCF" / "VSM"). */
JCE_API const char *jce_shadow_mode_name(JceShadowMode mode);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADOW_FILTER_H */
