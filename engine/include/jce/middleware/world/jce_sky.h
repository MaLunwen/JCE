/*
 * jce_sky.h -- Analytic Preetham daylight sky model.
 *
 * Pure-CPU module that, given a sun direction and an atmospheric
 * turbidity, evaluates the Preetham et al. closed-form sky-luminance /
 * chromaticity model ("A Practical Analytic Model for Daylight",
 * SIGGRAPH 1999) and returns the linear-RGB radiance for any view
 * direction in the upper hemisphere.
 *
 * Unlike Hosek-Wilkie (which ships a fitted coefficient dataset), the
 * Preetham model is fully closed-form: the 5 Perez distribution
 * coefficients (A..E) and the zenith values are polynomials in
 * turbidity and the sun-zenith angle.  That makes it testable on the
 * CPU with no GPU, no data tables, and no external dependencies — only
 * <math.h>.  The fs_sky.sc fragment shader mirrors this exact math so
 * the GPU sky is a bit-faithful twin of the tested CPU core (same
 * contract as jce_water.c ↔ vs_water.sc).
 *
 *   Perez distribution function:
 *       F(theta, gamma) = (1 + A * exp(B / cos(theta)))
 *                       * (1 + C * exp(D * gamma) + E * cos(gamma)^2)
 *   where
 *       theta  = angle between the view direction and the zenith,
 *       gamma  = angle between the view direction and the sun.
 *
 *   Radiance for each xyY channel:
 *       Q(theta, gamma) = Q_zenith * F(theta, gamma) / F(0, theta_s)
 *   where theta_s is the sun-zenith angle.  Y is luminance; x, y are the
 *   CIE chromaticity coordinates.  We assemble xyY, convert xyY -> XYZ
 *   -> linear sRGB, scale by exposure, and clamp to >= 0.
 *
 * This module is intentionally renderer-agnostic: it never touches bgfx
 * or the scene renderer.  Callers either feed the result into their own
 * shader uniforms or let jce_scene_renderer drive the sky pass from it.
 *
 * Layer: World (Layer 3) — public, pure math.
 */

#ifndef JCE_SKY_H
#define JCE_SKY_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Configuration for the analytic sky.  Defaults match a clear temperate
 * day; callers can crank turbidity up for a hazy / urban look. */
typedef struct {
    float turbidity;   /* atmospheric haze: ~1 = pristine, 2-3 = clear,
                          6+ = hazy/urban.  Clamped to [1, 10].          */
    float exposure;    /* linear output scale applied to the radiance.    */
    int   normalize;   /* if non-zero, divide radiance by the zenith
                          luminance so the absolute scale is independent
                          of turbidity (handy for authoring).            */
} JceSkyConfig;

/* Returns the default configuration (turbidity ~2.5, exposure 1, no
 * normalize). */
JCE_API JceSkyConfig jce_sky_config_default(void);

/* Precomputed Preetham state for one sun position.  The Perez
 * coefficient arrays are laid out [A, B, C, D, E] for the luminance Y
 * and the two CIE chromaticity channels x and y.  Yz/xz/yz are the
 * zenith values.  sun_dir is the unit vector TOWARD the sun (same
 * convention as JceTimeOfDayState.sun_direction). */
typedef struct {
    float perezY[5];   /* A..E for luminance Y                            */
    float perezx[5];   /* A..E for CIE chromaticity x                     */
    float perezy[5];   /* A..E for CIE chromaticity y                     */
    float Yz;          /* zenith absolute luminance (kcd/m^2)            */
    float xz;          /* zenith chromaticity x                          */
    float yz;          /* zenith chromaticity y                          */
    float sun_dir[3];  /* unit vector toward the sun                      */
    float exposure;    /* copied from config (so radiance() is self-
                          contained)                                      */
    float normalize;   /* 0 or 1 (copied from config)                    */
} JceSkyState;

/* Compute the Perez coefficients + zenith values from `cfg->turbidity`
 * and the sun-zenith angle implied by `sun_dir` (sun_dir[1] is the up
 * component → theta_s = acos(up)).  NULL cfg → defaults.  Robust to a
 * sun below the horizon: the sun-zenith angle is clamped just below the
 * horizon so the zenith / normalization terms stay finite. */
JCE_API JceSkyState jce_sky_evaluate(const JceSkyConfig *cfg,
                                     const float         sun_dir[3]);

/* Evaluate the sky radiance for a unit `view_dir`, writing linear RGB
 * into `out_rgb`.  theta is the view-zenith angle (clamped just below
 * pi/2 so the 1/cos(theta) horizon term never blows up); gamma is the
 * angle between view_dir and the sun.  Deterministic pure math
 * (<math.h> only); output is finite and >= 0. */
JCE_API void jce_sky_radiance(const JceSkyState *st,
                              const float        view_dir[3],
                              float              out_rgb[3]);

/* ── Sky-derived ambient (spherical harmonics) ──────────────────────
 *
 * The Lambertian cosine kernel is so low-frequency that projecting the
 * environment onto 3 SH bands (9 coefficients) reproduces diffuse irradiance
 * with about 1% average error (Ramamoorthi & Hanrahan 2001).  That makes
 * sky-derived ambient affordable everywhere: 9 vec3 uniforms, no sampler, no
 * view id, no cubemap, and no GPU work at all.
 *
 * This is what replaces hand-picked ambient constants.  A literal like
 * (0.25, 0.45, 0.80) is somebody approximating the sky by eye; these
 * coefficients ARE the sky the renderer is drawing, so the two cannot drift.
 *
 * Only the UPPER hemisphere is projected.  Sky light genuinely comes from
 * above, and leaving the lower hemisphere at zero is what stops SH ambient
 * from lighting the underside of everything like open sky.  Ground bounce is
 * a separate term and does not belong in a sky projection.
 *
 * Coefficients are stored pre-convolved with the cosine kernel (the
 * per-band constants pi, 2pi/3, 0, pi/4), so reconstruction is 9 multiply-adds
 * and nothing else.  Output is divided by pi, i.e. it is the radiance a white
 * Lambertian surface would emit -- which is what an "ambient colour" means. */

/* Project the sky onto 9 SH coefficients per RGB channel. */
JCE_API void jce_sky_project_sh9(const JceSkyState *st, float out_sh9[9][3]);

/* Reconstruct ambient radiance for a surface normal.  `n` need not be
 * normalised.  Output is clamped to >= 0: an L2 fit can ring slightly
 * negative, and negative light is never correct. */
JCE_API void jce_sky_irradiance_sh9(const float sh9[9][3],
                                    const float n[3],
                                    float       out_rgb[3]);

JCE_EXTERN_C_END

#endif /* JCE_SKY_H */
