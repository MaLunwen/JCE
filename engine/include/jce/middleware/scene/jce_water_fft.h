/*
 * jce_water_fft.h -- Tessendorf statistical FFT ocean (deep-water spectrum).
 *
 * The Gerstner model in jce_water.h sums a handful of analytic trochoidal waves;
 * this module instead synthesizes a STATISTICAL ocean surface from the Phillips
 * spectrum via an inverse Fast Fourier Transform (Tessendorf 2001).  A tiling
 * `N`x`N` patch of side `patch_size` is generated on the CPU: a frequency-domain
 * field h(k,t) is evolved analytically in time and an inverse 2D FFT brings it to
 * the spatial domain, producing a real height field plus two horizontal
 * displacement fields (the "choppy" / Gerstner-like XZ roll).  The result tiles
 * seamlessly and can be uploaded to a GPU displacement texture, or sampled on the
 * CPU for buoyancy.
 *
 * This is dependency-light: only the C math library + the engine allocator.  No
 * bgfx, no flecs, no globals.  A given (N, patch_size, wind, amplitude, seed) is
 * fully deterministic across runs and platforms — the seeded Gaussian spectrum
 * and the hand-rolled radix-2 FFT use no platform RNG and no undefined ordering,
 * so two creates with the same seed yield byte-identical fields, and evolve(t) is
 * a pure function of (state, t).
 *
 * ── The Tessendorf model (the equations this module implements) ─────────
 *  Wavevector for grid index (m,n), m,n in [-N/2, N/2):
 *      k = ( 2*PI*m / L , 2*PI*n / L ),   |k| = sqrt(kx^2 + kz^2)
 *
 *  Phillips spectrum (energy at wavevector k):
 *      L_w     = wind_speed^2 / g                 (largest wave from wind)
 *      Ph(k)   = amplitude * exp(-1/(|k|*L_w)^2) / |k|^4 * |dot(khat, windhat)|^2
 *                * exp(-|k|^2 * l^2)              (small-wave damping, l = L_w/1000)
 *      Ph(0)   = 0
 *
 *  Initial Fourier amplitudes (h0), with (xi_r, xi_i) ~ N(0,1) Gaussian:
 *      h0(k)   = (1/sqrt(2)) * (xi_r + i*xi_i) * sqrt(Ph(k))
 *
 *  Dispersion (deep water) and time evolution:
 *      w(k)    = sqrt(g * |k|)
 *      h(k,t)  = h0(k) * exp( i*w*t) + conj(h0(-k)) * exp(-i*w*t)
 *
 *  Inverse FFT of h(k,t) gives the real height field.  Horizontal displacement
 *  ("choppy waves") uses the gradient direction:
 *      D(k)    = -i * (k / |k|) * h(k,t)          (component-wise for X and Z)
 *  whose inverse FFTs give disp_x / disp_z.  A (-1)^(x+z) sign flip (equivalent
 *  to an fftshift) recentres the patch so it tiles seamlessly.
 *
 * Layer: Middleware / Scene (Layer 4).
 */

#ifndef JCE_WATER_FFT_H
#define JCE_WATER_FFT_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Opaque Tessendorf FFT ocean state.  Holds the immutable initial spectrum
 * (h0 and conj(h0(-k))), the per-frame FFT scratch, and the three output
 * spatial-domain fields (height, disp_x, disp_z).  Create -> evolve(t) (per
 * frame) -> sample / read field data -> destroy. */
typedef struct JceWaterFft JceWaterFft;

/* Create an FFT ocean.  `N` MUST be a power of two (validated); typical 64/128.
 * `patch_size` is the world-space side length of the tiling patch (>0).
 * `wind_speed` (m/s, >0) sets the dominant wavelength; (wind_dir_x, wind_dir_z)
 * is the wind direction (normalized internally; zero-length -> +X). `amplitude`
 * (>0) scales the Phillips energy (overall wave height).  `seed` drives the
 * deterministic Gaussian spectrum.  Returns NULL on non-power-of-two N,
 * non-positive patch_size, or allocation failure.  The created state is fully
 * deterministic in (N, patch_size, wind_speed, wind_dir, amplitude, seed). */
JCE_API JceWaterFft *JCE_CALL
jce_water_fft_create(int N, float patch_size,
                     float wind_speed, float wind_dir_x, float wind_dir_z,
                     float amplitude, unsigned int seed);

/* Free all buffers owned by `fft`.  NULL-safe. */
JCE_API void JCE_CALL jce_water_fft_destroy(JceWaterFft *fft);

/* Evolve the spectrum to absolute time `t` (seconds) and run the inverse 2D FFT,
 * refreshing the height / disp_x / disp_z fields.  Pure function of (state, t):
 * calling twice with the same t yields identical fields.  NULL-safe (no-op). */
JCE_API void JCE_CALL jce_water_fft_evolve(JceWaterFft *fft, float t);

/* Bilinearly sample the tiling height field at world (world_x, world_z), wrapping
 * into [0, patch_size).  Mirrors jce_water_sample_height's role for buoyancy:
 * returns the displaced surface height (no base plane added).  Returns 0 for a
 * NULL state or before the first evolve.  Deterministic. */
JCE_API float JCE_CALL
jce_water_fft_sample_height(const JceWaterFft *fft, float world_x, float world_z);

/* Grid resolution N (power of two).  0 for NULL. */
JCE_API int JCE_CALL jce_water_fft_resolution(const JceWaterFft *fft);

/* World-space patch side length the fields tile over.  0 for NULL. */
JCE_API float JCE_CALL jce_water_fft_patch_size(const JceWaterFft *fft);

/* Pointers to the N*N spatial-domain fields (row-major, index = z*N + x), valid
 * until the next evolve or destroy.  height is world-Y displacement; disp_x /
 * disp_z are the horizontal (choppy) displacements.  NULL for a NULL state. */
JCE_API const float *JCE_CALL jce_water_fft_height_data(const JceWaterFft *fft);
JCE_API const float *JCE_CALL jce_water_fft_disp_x_data(const JceWaterFft *fft);
JCE_API const float *JCE_CALL jce_water_fft_disp_z_data(const JceWaterFft *fft);

JCE_EXTERN_C_END

#endif /* JCE_WATER_FFT_H */
