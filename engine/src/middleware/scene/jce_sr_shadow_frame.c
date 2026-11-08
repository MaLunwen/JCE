/* Canonical frame shadow admission and cascade split upload. */
#include "jce_sr_shadow_frame.h"
#include "renderer/jce_render_encoder.h"

SrFrameShadowSelection sr_frame_shadow_selection(const JceSceneRenderer *sr)
{
    SrFrameShadowSelection result = {false, NULL};
    if (!sr) return result;
    static JceCvar *shadows;
    if (!shadows)
        shadows = jce_cvar_register_bool("r.shadows", true, JCE_CVAR_FLAG_NONE,
            "Directional + cascade shadows. 0 forces every surface fully "
            "lit (exact ablation: zeroes u_csmSplits).");
    result.active = (!shadows || jce_cvar_get_bool(shadows)) && sr->frame_shadow_active;
    if (result.active && sr->shadow_use_csm && sr->last_csm_valid)
        result.csm = &sr->last_csm;
    return result;
}

void sr_pack_csm_splits(const float *splits, uint32_t cascade_count,
                        float out[4])
{
    uint32_t n = 0;
    if (splits) {
        for (; n < cascade_count && n < 4u; n++) out[n] = splits[n + 1];
    }
    /* No cascades at all is a legitimate state -- the shader's own first line
     * is `if (u_csmSplits.x <= 0.0) return 1.0;`, i.e. zero means "no shadow
     * map", which is a different statement from "shadows end here". Padding
     * that case with anything positive would invent a shadow distance. */
    for (uint32_t i = n; i < 4u; i++) out[i] = (n > 0u) ? out[n - 1u] : 0.0f;
}

void sr_bind_frame_shadow_splits(JceSceneRenderer *sr)
{
    if (!sr) return;
    const SrFrameShadowSelection frame = sr_frame_shadow_selection(sr);
    float splits[4];
    sr_pack_csm_splits(frame.csm ? frame.csm->splits : NULL,
        frame.csm ? frame.csm->cascade_count : 0u, splits);
    jce_enc_set_uniform(sr->u_csm_splits, splits, 1u);
}
