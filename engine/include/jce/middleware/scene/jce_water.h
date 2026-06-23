/*
 * jce_water.h -- Pure Gerstner / sum-of-sines water surface model.
 *
 * Dependency-light (math only): given an array of N Gerstner waves, a base
 * water plane height, a world-space (x, z) and a time t, it computes the
 * displaced surface position and analytic normal.  No allocation, no globals,
 * no RNG; a given (waves, x, z, t) is fully deterministic and identical across
 * runs and platforms.  This is the CPU twin of the (next-phase) water vertex
 * shader, so the equation below is the single source of truth both must match
 * bit-for-bit.
 *
 * ── Exact wave equation ────────────────────────────────────────────────
 * For each wave i with amplitude A_i, wavelength L_i, speed S_i, horizontal
 * direction (Dx_i, Dz_i) (NOT required to be unit; it is normalized here) and
 * steepness Q_i in [0,1]:
 *
 *     k_i   = 2*PI / L_i                  (angular wavenumber; L_i <= 0 -> wave skipped)
 *     w_i   = k_i * S_i                   (temporal angular frequency)
 *     d_i   = normalize(Dx_i, Dz_i)       (zero-length dir -> wave skipped)
 *     phase = k_i * dot(d_i, (x, z)) + w_i * t
 *
 *   The Gerstner (trochoidal) displacement of the flat point P0 = (x, base_y, z):
 *
 *     X = x + sum_i  Q_i * A_i * d_i.x * cos(phase_i)
 *     Y = base_y + sum_i           A_i        * sin(phase_i)
 *     Z = z + sum_i  Q_i * A_i * d_i.z * cos(phase_i)
 *
 *   jce_water_sample_height returns only Y above (the vertical/sine sum); it
 *   does NOT apply the horizontal Gerstner roll (that is the displacement call).
 *
 * ── Analytic normal ────────────────────────────────────────────────────
 *   Using the partial derivatives of the displaced surface, the unnormalized
 *   normal is:
 *
 *     N.x = - sum_i  d_i.x * (k_i * A_i) * cos(phase_i)
 *     N.z = - sum_i  d_i.z * (k_i * A_i) * cos(phase_i)
 *     N.y = 1 - sum_i  Q_i * (k_i * A_i) * sin(phase_i)
 *
 *   then normalized to unit length.  For physical (sum Q_i*k_i*A_i < 1) waves
 *   N.y stays positive, so the surface never folds over.
 *
 * Layer: Middleware / Scene (Layer 4).
 */

#ifndef JCE_WATER_H
#define JCE_WATER_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* One Gerstner wave.  `dir_x`/`dir_z` need not be normalized (done internally).
 * `steepness` in [0,1] controls the horizontal Gerstner roll (0 = pure sine). */
typedef struct JceWaterWave {
    float amplitude;   /* crest-to-mid height (world units)             */
    float wavelength;  /* peak-to-peak distance (world units); >0       */
    float speed;       /* phase speed (world units / second)            */
    float dir_x;       /* horizontal travel direction X (normalized in) */
    float dir_z;       /* horizontal travel direction Z                 */
    float steepness;   /* Gerstner sharpness Q in [0,1]                 */
} JceWaterWave;

/* Sum-of-sines surface height (world Y) at (x, z) and time t.  Returns base_y
 * plus the vertical sine sum of the first `n` waves (see header equation).
 * Deterministic; n<=0 or NULL waves returns base_y. */
JCE_API float JCE_CALL
jce_water_sample_height(const JceWaterWave *waves, int n,
                        float base_y, float x, float z, float t);

/* Full Gerstner displacement (moves X and Z as well as Y) of the flat point
 * (x, base_y, z) at time t.  Writes the displaced world position into out_xyz
 * (must point to 3 floats).  Deterministic. */
JCE_API void JCE_CALL
jce_water_sample_displacement(const JceWaterWave *waves, int n,
                              float base_y, float x, float z, float t,
                              float *out_xyz);

/* Analytic, unit-length surface normal at (x, z) and time t (see header
 * equation).  Writes a normalized normal into out_xyz (3 floats); for
 * physical waves N.y > 0.  Deterministic; degenerate input writes (0,1,0). */
JCE_API void JCE_CALL
jce_water_sample_normal(const JceWaterWave *waves, int n,
                        float x, float z, float t, float *out_xyz);

/* ── Buoyancy force model (gap 2.3, slice 3) ─────────────────────────────
 * Mass-independent vertical buoyancy + linear drag for a body floating on the
 * water surface.  Pure math — no allocation, no globals, no RNG — so a given
 * (depth, velocity, strength, drag) is fully deterministic across runs and
 * platforms.  The runtime (jce_runtime_step) samples the active water surface
 * Y at the body's XZ via jce_water_sample_height, derives the submersion depth
 * = max(0, surface_y - body_y), and feeds it here once per fixed tick.
 *
 *   submersion = max(0, water_y - body_y)             (>0 only when submerged)
 *   F_buoy     = strength * submersion                (upward, +Y; Archimedes
 *                                                      analogue — deeper => more)
 *   F_drag     = -drag * vel_y * submersion           (resists vertical motion;
 *                                                      gated by submersion so an
 *                                                      airborne body keeps its
 *                                                      ballistic fall untouched)
 *   F_total    = F_buoy + F_drag
 *
 * The force is mass-independent (an authoring convenience: a body released
 * above water falls under gravity, enters, decelerates, and settles oscillating
 * about the surface without the designer hand-tuning per-mass coefficients).
 * Returns 0 when submersion <= 0 (body fully above water) so non-buoyant /
 * airborne bodies are provably unaffected.  `strength`/`drag` < 0 are clamped
 * to 0 (a negative buoyancy field would sink bodies — not this model's job). */
JCE_API float JCE_CALL
jce_water_buoyancy_force(float submersion, float vel_y,
                         float strength, float drag);

JCE_EXTERN_C_END

#endif /* JCE_WATER_H */
