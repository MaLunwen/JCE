/*
 * jce_ocean_spectrum.h -- modern ocean wave spectra (JONSWAP / PM / TMA +
 * Donelan-Banner directional spreading).
 *
 * Replaces the raw Phillips spectrum used by jce_water_fft: Phillips has no
 * fetch term, no independent peak-frequency control, and a k^-4 tail that never
 * converges (which is why every Phillips implementation bolts an ad-hoc
 * exp(-k^2 l^2) patch onto it).  The spectra here are the ones oceanography and
 * production renderers actually use.
 *
 * Pure math: no allocation, no I/O, no globals, no per-frame cost.  These
 * functions are called ONCE, while the initial Fourier amplitudes h0(k) are
 * built; the time evolution afterwards never touches this module.
 *
 * -- Factorised form -------------------------------------------------------
 * A non-directional frequency spectrum S(w) [m^2 s] is separated from a
 * normalised directional spreading D(w,theta) [1/rad], with
 *
 *     amplitude(k) ~ sqrt( (4*PI / (L * k)) * S(w) * D(w,theta) * |dw/dk| )
 *
 * where L is the FFT patch side.  D integrates to 1 over theta in [-PI,PI] for
 * every w, so changing the spreading redistributes energy in direction without
 * changing the total sea state -- the property that makes the two controls
 * independent for artists.
 *
 * -- S(w) ------------------------------------------------------------------
 * JONSWAP (Hasselmann et al. 1973):
 *     S(w)  = (alpha * g^2 / w^5) * exp(-1.25 * (wp/w)^4) * gamma^r
 *     alpha = 0.076 * (U^2 / (F*g))^0.22
 *     wp    = 22 * (g^2 / (U*F))^(1/3)
 *     gamma = 3.3
 *     r     = exp( -(w - wp)^2 / (2 * sigma^2 * wp^2) ),  sigma = 0.07 (w<=wp)
 *                                                                 0.09 (w> wp)
 * U is wind speed at 10 m [m/s]; F is FETCH [m], the distance the wind has
 * blown over.  Fetch is the control Phillips lacks: it is what turns a wind
 * speed into an actual sea state (a 20 m/s gale over a 2 km lake is chop, over
 * 500 km of open ocean it is a storm sea).
 *
 * PIERSON-MOSKOWITZ is the same expression with gamma = 1 (no peak
 * enhancement).  TMA is JONSWAP times the Kitaigorodskii depth-attenuation
 * factor phi(w,h), for shelf/coastal water where the bottom limits the
 * long-wave energy.
 *
 * -- D(w,theta) ------------------------------------------------------------
 * Donelan-Banner spreading, with an optional swell term folded in, then blended
 * against the isotropic density:
 *
 *     D = (1 - delta) * (1/(2*PI)) + delta * D_directional
 *
 * delta = `directional_blend` in [0,1]: 0 is a perfectly isotropic sea (useful
 * as a debug baseline), 1 is the full model.
 *
 * -- Determinism -----------------------------------------------------------
 * Integer-free, branch-stable, double-precision internally; every call is a
 * pure function of its arguments.  Two runs with identical inputs produce
 * bit-identical outputs.  Every entry point tolerates NULL and degenerate
 * arguments and is guaranteed to return a finite, non-negative value.
 */

/* PRIVATE to the scene layer for now.  It becomes public when the FFT ocean
 * actually consumes it -- publishing an API before its consumer exists commits
 * to a shape no real caller has exercised.
 */

#ifndef JCE_OCEAN_SPECTRUM_H
#define JCE_OCEAN_SPECTRUM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JceOceanSpectrumType {
    JCE_OCEAN_SPECTRUM_JONSWAP = 0, /* fetch-limited wind sea (default)        */
    JCE_OCEAN_SPECTRUM_PM      = 1, /* fully-developed limit: gamma = 1        */
    JCE_OCEAN_SPECTRUM_TMA     = 2  /* JONSWAP * Kitaigorodskii depth factor   */
} JceOceanSpectrumType;

typedef struct JceOceanSpectrumParams {
    JceOceanSpectrumType type;
    float wind_speed;        /* U10 [m/s]  -- clamped to [0.01, 500]           */
    float fetch;             /* F   [m]    -- clamped to [1, 1e9]              */
    float depth;             /* h   [m]    -- TMA only; <= 0 means deep water  */
    float gravity;           /* g   [m/s^2]-- clamped to [0.01, 1000]          */
    float swell;             /* xi  [0,1]  -- 0 isotropic ... 1 parallel trains*/
    float directional_blend; /* delta [0,1]-- 0 isotropic ... 1 full model     */
} JceOceanSpectrumParams;

/* Fills `p` with a temperate open-ocean sea state: JONSWAP, 10 m/s wind over
 * 100 km of fetch, which yields wp ~ 1.0 rad/s (period ~6.2 s) and a
 * significant wave height of roughly 2.2 m.  NULL is a no-op. */
void jce_ocean_spectrum_params_default(JceOceanSpectrumParams *p);

/* Angular peak frequency wp [rad/s] of the configured sea state.  Always
 * finite and > 0; returns 0 for NULL.  Exposed because a builder needs it to
 * size its wavenumber band and to drive foam/whitecap thresholds. */
float jce_ocean_spectrum_peak_omega(const JceOceanSpectrumParams *p);

/* Non-directional energy density S(omega) [m^2 s].  Non-negative and finite for
 * every omega, including omega <= 0 (returns 0: there is no energy at or below
 * zero frequency) and omega -> huge.  Returns 0 for NULL. */
float jce_ocean_spectrum_energy(const JceOceanSpectrumParams *p, float omega);

/* Directional spreading D(omega, theta) [1/rad], theta measured from the wind
 * direction and wrapped into [-PI, PI].  Symmetric in theta and normalised:
 * the integral over theta in [-PI, PI] is 1 for every omega.  Degenerate omega
 * falls back to the isotropic density 1/(2*PI) rather than 0, so the
 * normalisation invariant holds everywhere.  Returns 0 for NULL. */
float jce_ocean_spectrum_spreading(const JceOceanSpectrumParams *p,
                                   float omega, float theta);

/* Convenience: per-wavevector amplitude under the deep-water dispersion
 * w = sqrt(g*k), i.e.
 *
 *     sqrt( (4*PI / k) * S(w) * D(w,theta) * |dw/dk| ),   dw/dk = g / (2*w)
 *
 * The 1/sqrt(L) patch-size factor is NOT applied (L is a property of the FFT
 * grid, not of the sea state): the caller forms
 *
 *     h0(k) = (xi_r + i*xi_i) / sqrt(2) * jce_ocean_spectrum_amplitude(...) /
 *             sqrt(patch_size)
 *
 * with (xi_r, xi_i) ~ N(0,1).  `wind_dir_*` need not be normalised; a
 * zero-length wind direction is treated as +X.  Returns 0 at k = 0 and for
 * NULL. */
float jce_ocean_spectrum_amplitude(const JceOceanSpectrumParams *p,
                                   float kx, float kz,
                                   float wind_dir_x, float wind_dir_z);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* JCE_OCEAN_SPECTRUM_H */