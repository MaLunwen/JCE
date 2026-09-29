/*
 * jce_sr_veg_material.c  VegetationScatter albedo / tint override.
 *
 * Split out of jce_sr_environment.c, which is one of the size-frozen files
 * and sat AT its baseline: the ratchet is not asking for shorter comments,
 * it is asking for this.
 */

#include "jce_sr_veg_material.h"

/* VegetationScatter.albedo_path ("optional albedo override") and .tint
 * ("multiplied into instance base color"): both authored, both read by
 * nothing, so a scatter drew the mesh's own material at full white.  Zero
 * tint is an un-authored component, not black. */
bool sr_veg_apply_material(JceSceneRenderer *sr,
                                  const JceVegetationScatterComponent *vs,
                                  JcePbrMaterial *m)
{
    if (!sr || !vs || !m) return false;
    bool tint = vs->tint[0] > 0.0f || vs->tint[1] > 0.0f || vs->tint[2] > 0.0f;
    if (!vs->albedo_path[0] && !tint) return false;
    if (vs->albedo_path[0]) {
        JceTexture at = sr_resolve_texture(sr, vs->albedo_path);
        if (jce_texture_valid(at)) m->albedo_map = at;
    }
    if (tint) {
        m->base_color_factor[0] *= vs->tint[0];
        m->base_color_factor[1] *= vs->tint[1];
        m->base_color_factor[2] *= vs->tint[2];
    }
    return true;
}

/* Model paths submit immediately, so they arm the renderer-wide override and
 * clear it right after -- never across a return: this draw has a dozen early
 * exits and the first cut of this missed nine of them, and an override left
 * armed repaints whatever draws next. */
bool sr_veg_arm_material(JceSceneRenderer *sr,
                                const JceVegetationScatterComponent *vs,
                                JcePbrMaterial *out)
{
    *out = jce_pbr_material_default();
    if (!sr_veg_apply_material(sr, vs, out)) return false;
    jce_model_set_material_override(out);
    return true;
}
