/*
 * jce_skinned_morph_bind.c  Pack top-N morph weights into vec4 arrays.
 */

#include <jce/renderer/jce_skinned_morph_bind.h>

#include <string.h>

void jce_morph_bind_clear(JceMorphBindDesc *d)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
}

bool jce_morph_bind_build(const JceMorphSet *set, const float *weights,
                            uint32_t max, JceMorphBindDesc *out)
{
    if (!out) return false;
    jce_morph_bind_clear(out);
    if (max == 0) return false;
    if (max > JCE_MORPH_BIND_MAX) max = JCE_MORPH_BIND_MAX;
    /* Round down to multiple of 4 so vec4 slot count is exact. */
    max &= ~3u;
    if (max == 0) return false;

    float   tmp_w[JCE_MORPH_BIND_MAX];
    uint8_t tmp_i[JCE_MORPH_BIND_MAX];
    uint32_t n = jce_skinned_pack_morph_weights(set, weights,
                                                  tmp_w, tmp_i, max);
    if (n == 0 && (!set || !weights)) return false;

    /* Pack into the vec4 layout — same scalar order, just two parallel
     * float arrays. */
    for (uint32_t i = 0; i < max; ++i) {
        out->weights_vec4[i] = (i < n) ? tmp_w[i] : 0.0f;
        out->indices_vec4[i] = (i < n) ? (float)tmp_i[i] : 0.0f;
    }
    out->active_count    = n;
    out->vec4_slot_count = (n + 3) / 4;
    return true;
}
