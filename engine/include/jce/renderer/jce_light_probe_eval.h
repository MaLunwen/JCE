/*
 * jce_light_probe_eval.h  Spherical-harmonic L1/L2 evaluation.
 *
 * `jce_gi_probes.h` stores per-cell SH3 coefficients (9 floats per
 * colour channel = 27 floats per probe).  This module is the
 * normal→colour evaluator the forward shader's host counterpart
 * uses to spot-check / pre-bake probe lookups.  The actual GPU side
 * binds the 9 vec3 coefficients as a single uniform buffer (B17.7).
 *
 * Convention: 2nd-order ZH/SH, normalised SH basis matching
 * Ramamoorthi/Hanrahan 2001 ("An Efficient Representation for
 * Irradiance Environment Maps").
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_LIGHT_PROBE_EVAL_H
#define JCE_LIGHT_PROBE_EVAL_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SH3_COEFFS 9

/* Evaluate the irradiance of a single colour channel at world-space
 * direction `n` given 9 SH coefficients `c`. */
JCE_API float jce_sh3_eval(const float c[JCE_SH3_COEFFS],
                            const float n[3]);

/* RGB variant — caller supplies three coefficient arrays (one per
 * channel).  Output is written to `out_rgb`. */
JCE_API void jce_sh3_eval_rgb(const float c_r[JCE_SH3_COEFFS],
                                const float c_g[JCE_SH3_COEFFS],
                                const float c_b[JCE_SH3_COEFFS],
                                const float n[3],
                                float out_rgb[3]);

/* Project a single radiance sample `L_rgb` from direction `d`
 * (unit vector) onto the 9-coef SH basis.  Useful for bake-time
 * accumulation: caller calls this for every captured radiance
 * sample then divides by the sample count at the end. */
JCE_API void jce_sh3_project_sample(const float L_rgb[3],
                                      const float d[3],
                                      float       c_r[JCE_SH3_COEFFS],
                                      float       c_g[JCE_SH3_COEFFS],
                                      float       c_b[JCE_SH3_COEFFS]);

/* Convolve with cosine kernel so the SH represents irradiance
 * rather than radiance.  Mutates in place. */
JCE_API void jce_sh3_convolve_cosine(float c[JCE_SH3_COEFFS]);

JCE_EXTERN_C_END

#endif /* JCE_LIGHT_PROBE_EVAL_H */
