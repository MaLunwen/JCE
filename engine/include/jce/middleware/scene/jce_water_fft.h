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

#include <stdbool.h>

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
/* One point on the parametric FFT surface.
 *
 * The surface is NOT a height map: vs_water.sc draws the vertex authored at
 * world (x,z) at (x + disp_x, base_y + height, z + disp_z).  Asking for "the
 * height at world p" therefore requires inverting that displacement, which is
 * what jce_water_fft_sample_surface does.  grid_x/grid_z are the recovered
 * authored coordinates -- the point the GPU actually drew at p. */
typedef struct {
    float height;    /* h at the recovered grid point (add base_y for world Y) */
    float grid_x;    /* recovered authored X */
    float grid_z;    /* recovered authored Z */
    float disp_x;    /* horizontal chop applied at that point */
    float disp_z;

    /* Filled from the auxiliary fields when they are enabled, so a caller gets
     * them for the price of the solve it already paid for.  Defaults are the
     * "undisturbed" answers, not zero: a zero Jacobian would read as a folded
     * surface and a zero gradient is a legitimate flat one, so only `jacobian`
     * needs the distinction.
     *
     *   slope_x/slope_z  exact dh/dx, dh/dz   (jce_water_fft_enable_slopes)
     *                    0 when slopes are off
     *   jacobian         fold determinant     (jce_water_fft_enable_foam)
     *                    1 when foam is off */
    float slope_x;
    float slope_z;
    float jacobian;
} JceWaterFftSample;

/* Solve x + D(x) = (world_x, world_z) by fixed-point iteration, then evaluate
 * the surface there.  `iterations` is clamped to [1,16]; 4 is the production
 * norm.  Fills `out` with zeros when the field has never been evolved. */
JCE_API void JCE_CALL jce_water_fft_sample_surface(const JceWaterFft *fft,
                                                   float world_x, float world_z,
                                                   int iterations,
                                                   JceWaterFftSample *out);

/* Convenience: the height component of jce_water_fft_sample_surface.
 *
 * Prefer this over jce_water_fft_sample_height for anything that must agree
 * with what is on screen.  The older function samples h(p) directly, which
 * ignores the horizontal chop and is therefore wrong by exactly that amount --
 * worst at crests, where buoyancy needs it most. */
JCE_API float JCE_CALL jce_water_fft_sample_height_displaced(
    const JceWaterFft *fft, float world_x, float world_z, int iterations);

JCE_API int JCE_CALL jce_water_fft_resolution(const JceWaterFft *fft);

/* World-space patch side length the fields tile over.  0 for NULL. */
JCE_API float JCE_CALL jce_water_fft_patch_size(const JceWaterFft *fft);

/* Pointers to the N*N spatial-domain fields (row-major, index = z*N + x), valid
 * until the next evolve or destroy.  height is world-Y displacement; disp_x /
 * disp_z are the horizontal (choppy) displacements.  NULL for a NULL state. */
JCE_API const float *JCE_CALL jce_water_fft_height_data(const JceWaterFft *fft);
JCE_API const float *JCE_CALL jce_water_fft_disp_x_data(const JceWaterFft *fft);
JCE_API const float *JCE_CALL jce_water_fft_disp_z_data(const JceWaterFft *fft);

/* Rebuild the initial spectrum from JONSWAP instead of raw Phillips.
 *
 * Phillips has no FETCH, so it cannot distinguish the same wind blowing across
 * a pond from the same wind blowing across an ocean -- which is the single
 * control that makes a sea look like a SPECIFIC sea.  `fetch` is that distance
 * in metres; `swell` in [0,1] adds a Horvath long-period component.
 *
 * OPT-IN ON PURPOSE.  Switching spectra changes every height value, so any
 * golden-hash or golden-image baseline over the water field is a deliberate
 * rebaseline -- never a side effect of a build.  Phillips stays the default.
 *
 * Returns false for a NULL field or one that has already been evolved: h0 is
 * the field's identity, and swapping it mid-flight would teleport every wave. */
JCE_API bool JCE_CALL jce_water_fft_use_jonswap(JceWaterFft *fft,
                                                float wind_speed, float fetch,
                                                float swell, unsigned int seed);

/* ── Exact surface gradient (Tessendorf eq. 37) ────────────────────────
 *
 * OPT-IN, because it costs two more inverse transforms per evolve -- a
 * two-thirds increase in FFT work -- and a caller that only needs heights
 * should not pay for gradients it never reads.
 *
 * Why not just central-difference the height field?  Because that is wrong
 * exactly where it shows.  A finite difference across a sharp crest straddles
 * the peak and reports a gentler slope than the surface really has, so the
 * specular highlight along a breaking wave -- the most visible part of an
 * ocean -- is the part the approximation damages most.  i*k*h is the analytic
 * derivative of the same series the heights came from, so it is exact at every
 * sample rather than only where the surface is smooth.
 *
 * NOTE the field named disp_x/disp_z is the horizontal CHOPPY DISPLACEMENT
 * (-i*(k/|k|)*h), not a gradient; the two differ by using the unit wavevector
 * versus the full one, and neither can be derived from the other.
 *
 * Enabling re-evolves at the current time, so the slope fields are valid the
 * moment the call returns.  Returns false only if the allocation failed, in
 * which case the field is untouched and slopes stay off. */
JCE_API bool JCE_CALL jce_water_fft_enable_slopes(JceWaterFft *fft);

/* N*N spatial dh/dx and dh/dz, row-major, valid until the next evolve.
 * NULL when slopes were never enabled -- which is a deliberate signal to the
 * caller rather than a silently-zero gradient field. */
JCE_API const float *JCE_CALL jce_water_fft_slope_x_data(const JceWaterFft *fft);
JCE_API const float *JCE_CALL jce_water_fft_slope_z_data(const JceWaterFft *fft);

/* ── Jacobian foam ─────────────────────────────────────────────────────
 *
 * The determinant of the horizontal map x -> x + D(x), per grid cell.
 *
 *   > 1  the surface is being stretched (a trough)
 *   ~ 1  undisturbed
 *   < 1  compressed -- the leading face of a steepening wave
 *   < 0  FOLDED over itself, which is physically what a breaking crest is
 *
 * This is why foam belongs here rather than in a hand-painted texture: it
 * appears where the surface actually breaks, so it moves with the sea state
 * instead of being scattered by a noise function that knows nothing about the
 * waves underneath it.  The renderer packs it into the displacement texture's
 * alpha channel, which is otherwise unused.
 *
 * OPT-IN, and cheap when on: it is finite differences over the displacement
 * fields that evolve already produced -- no extra transform.  (A spectral
 * derivative of D would cost four more inverse FFTs to sharpen a quantity that
 * is then thresholded anyway.)
 *
 * Enabling re-evolves at the current time so the field is valid immediately. */
JCE_API bool JCE_CALL jce_water_fft_enable_foam(JceWaterFft *fft);

/* N*N Jacobian values, row-major, valid until the next evolve.  NULL when foam
 * was never enabled -- a deliberate signal, not a silently-flat field. */
JCE_API const float *JCE_CALL jce_water_fft_foam_data(const JceWaterFft *fft);

JCE_EXTERN_C_END

#endif /* JCE_WATER_FFT_H */
