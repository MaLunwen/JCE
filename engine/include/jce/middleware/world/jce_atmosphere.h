/*
 * jce_atmosphere.h -- Physically based atmospheric transmittance.
 *
 * Pure-CPU, pure-C99, no renderer dependency.  Computes how much sunlight
 * survives the trip through the atmosphere to a given altitude and direction.
 *
 * WHY THIS EXISTS
 * ---------------
 * Sun colour used to be a hand-authored curve: a night tint lerped to a warm
 * dawn colour, to a near-white noon colour, to a deeper dusk red.  That curve
 * has to be re-tuned for every atmosphere and it cannot agree with a sky that
 * is drawn by a different model.  Here the reddening is the physics instead:
 *
 *     E_perp = E_top_of_atmosphere * T(altitude, sun_direction)
 *
 * At noon the optical path is short, T is near-neutral (~0.85, 0.90, 0.93) and
 * the light is near-white.  Near the horizon the path through the Rayleigh
 * layer is tens of times longer, blue is extinguished far faster than red, and
 * T collapses toward (0.4, 0.15, 0.03).  That ratio IS the orange sunset --
 * derived, with nothing authored.
 *
 * The same transmittance is what a GPU transmittance LUT would store, so a
 * higher tier can sample a LUT instead without changing any of the numbers.
 * The CPU path stays canonical: it needs no view id, no sampler and no GPU, so
 * it works unchanged on the single-core / no-discrete-GPU / WebGL2 floor.
 *
 * Layer: World (Layer 3) -- public.
 */

#ifndef JCE_ATMOSPHERE_H
#define JCE_ATMOSPHERE_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Medium parameters.  Scattering/absorption coefficients are per kilometre at
 * sea level; heights are kilometres.  Defaults are the standard Earth values
 * used by the production sky-atmosphere literature. */
typedef struct JceAtmosphereParams {
    float    planet_radius_km;         /* 6360 */
    float    atmosphere_height_km;     /* 60   */

    jce_vec3 rayleigh_scattering;      /* (5.802, 13.558, 33.100)e-3 */
    float    rayleigh_scale_height_km; /* 8.0  */

    float    mie_scattering;           /* 3.996e-3 */
    float    mie_extinction;           /* 4.440e-3 */
    float    mie_scale_height_km;      /* 1.2  */

    jce_vec3 ozone_absorption;         /* (0.650, 1.881, 0.085)e-3 */
    float    ozone_center_km;          /* 25.0 */
    float    ozone_width_km;           /* 30.0 (half-width of the tent) */

    /* Illuminance of the sun on a surface normal to it, ABOVE the atmosphere,
     * in lux.  ~128000 reproduces the measured ~105000 lx at ground level at
     * noon once transmittance is applied. */
    float    sun_illuminance_top;      /* 128000 */

    /* Below this sun elevation the transmittance lookup is clamped, so meshes
     * keep a visible (if dim) sun through the terminator instead of snapping
     * to black.  Degrees. */
    float    min_sun_elevation_deg;    /* -2.0 */

    /* Ray-march steps for one transmittance integral. */
    int      steps;                    /* 40 */
} JceAtmosphereParams;

JCE_API JceAtmosphereParams JCE_CALL jce_atmosphere_default_params(void);

/* Fraction of light surviving from `altitude_km` along `dir` to space, per
 * RGB channel, in [0,1].  `dir` need not be normalised; `up` is the world up
 * axis.  Returns (1,1,1) for a ray that never leaves the ground. */
JCE_API jce_vec3 JCE_CALL jce_atmosphere_transmittance(
    const JceAtmosphereParams *p, float altitude_km,
    jce_vec3 dir, jce_vec3 up);

/* Illuminance in lux of the sun on a surface facing it, at `altitude_km`.
 * This is E_top_of_atmosphere * transmittance, with the elevation clamped by
 * `min_sun_elevation_deg`.  Below the horizon it falls off smoothly to zero
 * rather than cutting. */
JCE_API jce_vec3 JCE_CALL jce_atmosphere_sun_illuminance(
    const JceAtmosphereParams *p, float altitude_km,
    jce_vec3 sun_dir, jce_vec3 up);

/* ── Precomputed tables ────────────────────────────────────────────────
 *
 * The transmittance integral above is a ray-march.  Evaluating it per pixel per
 * frame is affordable for ONE direction (the sun) and not for a sky dome, so
 * the standard approach -- Bruneton/Hillaire -- is to bake it once into a small
 * table parameterised by (altitude, sun cosine) and sample that instead.
 *
 * These bake on the CPU, deterministically, with no GPU and no compute shader.
 * That is not a compromise: the charter's minimum profile has no compute at all
 * on WebGL2, and a CPU-baked table is also the only version the LIGHTING
 * authority can read, since it runs headless.  Uploading the same table to the
 * GPU for the sky pass is then a pure transfer -- the two cannot disagree,
 * because there is only one table.
 *
 * Cost is a one-off at parameter change, not per frame: parameters change when
 * the time of day moves the sun, and the table is not a function of sun
 * direction -- it is indexed BY it. */

#define JCE_ATMOSPHERE_LUT_ALTITUDE  64u   /* rows: altitude 0..atmosphere top */
#define JCE_ATMOSPHERE_LUT_COSINE   256u   /* cols: cos(sun zenith) -1..1      */

/* Bake the transmittance table.
 *
 * `out_rgb` receives JCE_ATMOSPHERE_LUT_ALTITUDE * JCE_ATMOSPHERE_LUT_COSINE
 * RGB triples (3 floats each), row-major with cosine varying fastest.
 *
 * Row r covers altitude r/(ALTITUDE-1) * atmosphere_height_km; column c covers
 * mu = cos(zenith) mapped LINEARLY over [-1,1].  Linear in mu rather than in
 * the angle on purpose: the interesting variation is concentrated near the
 * horizon in ANGLE, but a shader samples with a dot product, and a table that
 * needs an acos to index is a table that costs a transcendental per sample.
 *
 * Returns false, touching nothing, on a NULL argument. */
JCE_API bool JCE_CALL jce_atmosphere_bake_transmittance_lut(
    const JceAtmosphereParams *p, float *out_rgb);

/* Sample the baked table with bilinear interpolation.
 *
 * Must agree with jce_atmosphere_transmittance to within the table's own
 * resolution -- that agreement is what makes "the GPU sky and the CPU lighting
 * authority see one atmosphere" true rather than merely intended. */
JCE_API jce_vec3 JCE_CALL jce_atmosphere_sample_transmittance_lut(
    const JceAtmosphereParams *p, const float *lut,
    float altitude_km, float mu);

/* Multiple-scattering table (Hillaire 2020, section 4).
 *
 * Single scattering alone makes a clear sky too dark near the horizon and
 * makes twilight collapse to black, because most of the light reaching the eye
 * at those angles has bounced more than once.  This bakes the second-and-beyond
 * order contribution as an infinite geometric series in one factor, which is
 * the whole reason the technique is cheap enough to be real-time.
 *
 * `out_rgb` receives JCE_ATMOSPHERE_MS_LUT_DIM^2 RGB triples: rows are
 * altitude, columns are cos(sun zenith), both linear.
 *
 * Returns false, touching nothing, on a NULL argument. */
#define JCE_ATMOSPHERE_MS_LUT_DIM 32u

JCE_API bool JCE_CALL jce_atmosphere_bake_multiscatter_lut(
    const JceAtmosphereParams *p, float *out_rgb);

JCE_API jce_vec3 JCE_CALL jce_atmosphere_sample_multiscatter_lut(
    const JceAtmosphereParams *p, const float *lut,
    float altitude_km, float mu);

/* ── GPU upload ─────────────────────────────────────────────────────────
 *
 * Pack a baked LUT into RGBA texel order for a GPU texture.  `out_rgba` holds
 * cells*4 floats; alpha is 1.
 *
 * The packing itself is trivial.  What is NOT trivial, and what the pair below
 * exists to pin, is the UV CONVENTION: the shader will sample this texture
 * with a normalised (u,v) and must land on the same entry the CPU authority
 * reaches with (altitude, mu).  Get the row/column order or the half-texel
 * offset wrong and the sky is smoothly, plausibly wrong -- the failure mode
 * that cannot be spotted by looking at it. */
JCE_API void JCE_CALL jce_atmosphere_pack_lut_rgba32f(const float *lut,
                                                      uint32_t cells,
                                                      float *out_rgba);

/* Map (altitude, mu) to the texture coordinates a shader must use.
 *
 * Texel CENTRES: a LUT is a set of samples, not a set of cells, so entry i of
 * N sits at (i + 0.5)/N.  Sampling at i/N instead shifts the whole table by
 * half a texel, which at the horizon -- where transmittance changes fastest --
 * is a visible colour shift rather than a rounding detail. */
JCE_API void JCE_CALL jce_atmosphere_lut_uv(const JceAtmosphereParams *p,
                                            float altitude_km, float mu,
                                            float *out_u, float *out_v);

/* Bilinear fetch from PACKED RGBA data using normalised (u,v) -- i.e. exactly
 * what the shader does.  Provided so the GPU path's arithmetic can be verified
 * headlessly against jce_atmosphere_sample_transmittance_lut; if the two
 * disagree, the sky and the lighting authority are reading two different
 * atmospheres again. */
JCE_API jce_vec3 JCE_CALL jce_atmosphere_sample_packed_rgba32f(
    const float *rgba, uint32_t width, uint32_t height, float u, float v);

JCE_EXTERN_C_END

#endif /* JCE_ATMOSPHERE_H */
