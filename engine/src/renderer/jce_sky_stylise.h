/*
 * jce_sky_stylise.h -- the stylisation layer.
 *
 * The design's split: the physical core computes RADIANCE, this grades it. Not
 * a different sky model at lower fidelity -- the same scattering result, put
 * through a colour grade. That ordering is what lets the lighting authority
 * derive sun colour from transmittance while the sky still looks authored.
 *
 * THE PROPERTY THAT MATTERS is acceptance criterion 7: at identity parameters
 * this must be provably a no-op. Not "close enough" -- bit-identical. A grade
 * that shifts the sky by half a unit when nobody asked for stylisation is a
 * permanent, global colour change that reads as the atmosphere model being
 * slightly wrong, and it would be debugged in the scattering code for a long
 * time before anyone suspected a disabled feature.
 *
 * Layer: Renderer (L3). Pure arithmetic, no bgfx. The GPU twin lives in
 * standard/sky_stylise.sh.
 */

#ifndef JCE_SKY_STYLISE_H
#define JCE_SKY_STYLISE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceSkyStylise {
    /* Palette remap: tint applied at the dark, mid and bright ends of the
     * radiance range. Identity is (1,1,1) for all three. */
    float shadow_tint[3];
    float mid_tint[3];
    float highlight_tint[3];

    /* Posterisation: quantise luminance into this many bands. 0 = off.
     * 1 would be a single flat band, which is not a stylisation but a bug that
     * looks like one -- it is treated as off. */
    uint32_t bands;

    /* Rim emphasis: brighten where the cloud silhouette meets the sky. 0 = off.
     * This is what makes stylised clouds read as shapes rather than as fog. */
    float rim_strength;

    /* Saturation multiplier around the original luminance. 1 = identity. */
    float saturation;
} JceSkyStylise;

/* Identity: every field set so that jce_sky_stylise_apply() returns its input
 * unchanged. Callers must start from this rather than from a zeroed struct --
 * a zeroed struct has zero tints, which is black. */
JceSkyStylise jce_sky_stylise_identity(void);

/* True when these parameters cannot change any colour, so the caller can skip
 * the work AND, more importantly, so the "is it off" question has exactly one
 * answer rather than one per call site. */
bool jce_sky_stylise_is_identity(const JceSkyStylise *s);

/*
 * Grade one radiance sample.
 *
 * `rim` is 0 in the interior of a cloud and 1 at its silhouette; pass 0 when
 * there is no cloud. `in_rgb` may alias `out_rgb`.
 *
 * A NULL or identity `s` copies the input verbatim -- see the header note on
 * why that has to be exact.
 */
void jce_sky_stylise_apply(const JceSkyStylise *s, const float in_rgb[3],
                           float rim, float out_rgb[3]);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SKY_STYLISE_H */
