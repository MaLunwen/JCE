/*
 * jce_rayleigh.h
 *
 * The Rayleigh scattering coefficient, and why the sky is blue.
 *
 * Sky radiance is what SCATTERS toward the eye:
 *
 *     L(view) = beta_R(lambda) * phase(theta) * <optical depth terms>
 *
 * beta_R is the wavelength-dependent part, and it is the ONLY place the blue
 * comes from -- everything else in that product is grey.  It goes as
 * lambda^-4, so short wavelengths scatter far more: at the sRGB primaries the
 * ratio blue:red is about 5.7:1.
 *
 * The failure this exists to prevent is subtle enough to have shipped: the sky
 * shader coloured itself with the TRANSMITTANCE table instead, which is
 * exp(-beta*s) -- what survives rather than what scatters.  Because beta is
 * largest for blue, transmittance is largest for RED, so the sky came out
 * WARM.  It still had a plausible gradient, still brightened near the horizon,
 * still responded to sun elevation; it was simply the wrong colour, in the
 * exact direction that looks like an artistic choice about time of day.
 *
 * Header-only and bgfx-free so the values are testable without a GPU: the
 * shader hard-codes the same normalised triple, and the test pins both.
 */
#ifndef JCE_RAYLEIGH_H
#define JCE_RAYLEIGH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Rayleigh scattering coefficient at sea level, per metre, at the sRGB
 * primaries (680 / 550 / 440 nm).  Standard atmosphere; the same figures
 * Bruneton/Nishita-style models use. */
#define JCE_RAYLEIGH_BETA_R 5.802e-6f
#define JCE_RAYLEIGH_BETA_G 13.558e-6f
#define JCE_RAYLEIGH_BETA_B 33.100e-6f

/* Rec.709 luminance weights -- the normalisation basis, see below. */
#define JCE_LUMA_R 0.2126f
#define JCE_LUMA_G 0.7152f
#define JCE_LUMA_B 0.0722f

/* Write beta_R normalised so that its LUMINANCE is exactly 1.
 *
 * Normalising by luminance rather than by max or by sum is what keeps this a
 * hue correction instead of an exposure change: the authored sky intensity
 * keeps meaning exactly what it meant before, and a scene that was correctly
 * exposed stays correctly exposed.  Normalising by the max would have darkened
 * every sky in every existing scene by ~2.4x, which would be read as "the fix
 * broke the lighting" and backed out. */
static inline void jce_rayleigh_beta_luma_normalised(float out_rgb[3])
{
    if (!out_rgb) return;
    const float lum = JCE_LUMA_R * JCE_RAYLEIGH_BETA_R
                    + JCE_LUMA_G * JCE_RAYLEIGH_BETA_G
                    + JCE_LUMA_B * JCE_RAYLEIGH_BETA_B;
    out_rgb[0] = JCE_RAYLEIGH_BETA_R / lum;
    out_rgb[1] = JCE_RAYLEIGH_BETA_G / lum;
    out_rgb[2] = JCE_RAYLEIGH_BETA_B / lum;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_RAYLEIGH_H */
