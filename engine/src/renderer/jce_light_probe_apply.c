/*
 * jce_light_probe_apply.c  Pack SH3 probes into a flat vec4 array.
 */

#include <jce/renderer/jce_light_probe_apply.h>

#include <string.h>

void jce_light_probe_bind_clear(JceLightProbeBindBlock *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
}

uint32_t jce_light_probe_pack_to_uniform(const JceLightProbeSh3Rgb *probes,
                                          uint32_t count,
                                          JceLightProbeBindBlock *out)
{
    if (!out) return 0;
    jce_light_probe_bind_clear(out);
    if (!probes || count == 0) return 0;
    if (count > JCE_LIGHT_PROBE_APPLY_MAX_PROBES)
        count = JCE_LIGHT_PROBE_APPLY_MAX_PROBES;

    for (uint32_t p = 0; p < count; ++p) {
        const JceLightProbeSh3Rgb *src = &probes[p];
        for (uint32_t c = 0; c < JCE_SH3_COEFFS; ++c) {
            float *dst = &out->uniforms[(p * JCE_SH3_COEFFS + c) * 4];
            dst[0] = src->r[c];
            dst[1] = src->g[c];
            dst[2] = src->b[c];
            dst[3] = 0.0f;
        }
    }
    out->probe_count = count;
    out->vec4_count  = count * JCE_SH3_COEFFS;
    return count;
}
