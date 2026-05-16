/*
 * jce_light_probe_apply.h  Pack SH3 light-probe coefficients into
 * the shader uniform layout the cluster forward pass consumes.
 *
 * Each probe contributes 9 SH coefficients per RGB channel (27
 * floats).  GPU layout flattens to 9 vec4 entries per probe:
 *     vec4[0..8] = (r_coef_i, g_coef_i, b_coef_i, pad)
 *
 * For N probes, the output uniform array is N * 9 vec4 entries laid
 * out contiguously (probe-major).  The cluster shader indexes by
 * probe_id × 9 + coef_index.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_LIGHT_PROBE_APPLY_H
#define JCE_LIGHT_PROBE_APPLY_H

#include <jce/renderer/jce_light_probe_eval.h>

JCE_EXTERN_C_BEGIN

#define JCE_LIGHT_PROBE_APPLY_MAX_PROBES 32

typedef struct {
    float r[JCE_SH3_COEFFS];
    float g[JCE_SH3_COEFFS];
    float b[JCE_SH3_COEFFS];
} JceLightProbeSh3Rgb;

typedef struct {
    /* probe_count * 9 vec4 entries, each (r, g, b, padding). */
    float    uniforms[JCE_LIGHT_PROBE_APPLY_MAX_PROBES * JCE_SH3_COEFFS * 4];
    uint32_t probe_count;
    uint32_t vec4_count;     /* probe_count * 9 */
} JceLightProbeBindBlock;

/* Pack N probes into the contiguous vec4 block.  Caller passes an
 * array of `count` JceLightProbeSh3Rgb (RGB coefficient sets).
 * Truncates at JCE_LIGHT_PROBE_APPLY_MAX_PROBES.  Returns the actual
 * number packed.  Padding component (.w) is always 0. */
JCE_API uint32_t jce_light_probe_pack_to_uniform(
    const JceLightProbeSh3Rgb *probes,
    uint32_t                    count,
    JceLightProbeBindBlock     *out);

/* Reset block — useful when no probes exist; shader still binds the
 * array each draw. */
JCE_API void jce_light_probe_bind_clear(JceLightProbeBindBlock *out);

JCE_EXTERN_C_END

#endif /* JCE_LIGHT_PROBE_APPLY_H */
