/*
 * jce_material_override.c  See jce_material_override.h for why this exists.
 *
 * The rule lived in three copies -- runtime loader, editor path-repair,
 * inspector material load -- and the first two were written without any
 * guard at all.  One copy means a renderer cannot lose an override through
 * whichever path happens to touch it.
 */

#include <jce/middleware/scene/jce_material_override.h>

void jce_mesh_renderer_to_pbr(const JceMeshRenderer *mr, JcePbrMaterial *out)
{
    if (!mr || !out) return;
    if (mr->base_color[3] > 0.0f) {
        out->base_color_factor[0] = mr->base_color[0];
        out->base_color_factor[1] = mr->base_color[1];
        out->base_color_factor[2] = mr->base_color[2];
        out->base_color_factor[3] = mr->base_color[3];
    }
    out->metallic_factor     = mr->metallic;
    out->roughness_factor    = mr->roughness;
    out->emissive_factor[0]  = mr->emissive[0];
    out->emissive_factor[1]  = mr->emissive[1];
    out->emissive_factor[2]  = mr->emissive[2];
    out->normal_scale        = mr->normal_scale;
    out->ao_strength         = mr->ao_strength;
    out->alpha_mode          = (JceAlphaMode)mr->alpha_mode;
    out->alpha_cutoff        = mr->alpha_cutoff;
    out->render_priority     = mr->render_priority;
    out->double_sided        = mr->double_sided;
    out->receive_shadows_off = mr->shadow_receive_off;
    out->uv_tiling[0]        = mr->uv_tiling[0];
    out->uv_tiling[1]        = mr->uv_tiling[1];
    out->uv_offset[0]        = mr->uv_offset[0];
    out->uv_offset[1]        = mr->uv_offset[1];
    out->blend_mode          = (JceBlendMode)mr->blend_mode;
    out->stencil_func        = (uint8_t)mr->stencil_func;
    out->stencil_ref         = (uint8_t)mr->stencil_ref;
    out->stencil_read_mask   = (uint8_t)mr->stencil_read_mask;
    out->stencil_fail_op     = (uint8_t)mr->stencil_fail_op;
    out->stencil_zfail_op    = (uint8_t)mr->stencil_zfail_op;
    out->stencil_pass_op     = (uint8_t)mr->stencil_pass_op;
    out->clearcoat           = mr->clearcoat;
    out->clearcoat_roughness = mr->clearcoat_roughness;
    out->anisotropy             = mr->anisotropy;
    out->anisotropy_rotation    = mr->anisotropy_rotation;
    out->translucency           = mr->translucency;
    out->translucency_thickness = mr->translucency_thickness;
    out->translucency_color[0]  = mr->translucency_color[0];
    out->translucency_color[1]  = mr->translucency_color[1];
    out->translucency_color[2]  = mr->translucency_color[2];
    out->sheen_color[0]      = mr->sheen_color[0];
    out->sheen_color[1]      = mr->sheen_color[1];
    out->sheen_color[2]      = mr->sheen_color[2];
    out->sheen_roughness     = mr->sheen_roughness;
}

void jce_mesh_renderer_apply_material_pbr(JceMeshRenderer *mr,
                                          const JcePbrMaterial *mat)
{
    if (!mr || !mat) return;

    const uint32_t ovr = mr->material_override_mask;

    if (!(ovr & JCE_MR_OVERRIDE_BASE_COLOR)) {
        mr->base_color[0] = mat->base_color_factor[0];
        mr->base_color[1] = mat->base_color_factor[1];
        mr->base_color[2] = mat->base_color_factor[2];
        mr->base_color[3] = mat->base_color_factor[3];
    }
    /* No override bit for the UV transform, deliberately: the override mask
     * exists so a per-instance TINT survives a shared material being saved,
     * and tiling is not a per-instance decision in any engine that has both.
     * If one is ever wanted, it needs its own bit -- borrowing another's
     * would make two fields un-overridable together. */
    mr->uv_tiling[0] = mat->uv_tiling[0];
    mr->uv_tiling[1] = mat->uv_tiling[1];
    mr->uv_offset[0] = mat->uv_offset[0];
    mr->uv_offset[1] = mat->uv_offset[1];
    mr->blend_mode   = (int)mat->blend_mode;
    mr->stencil_func      = (int)mat->stencil_func;
    mr->stencil_ref       = (int)mat->stencil_ref;
    mr->stencil_read_mask = (int)mat->stencil_read_mask;
    mr->stencil_fail_op   = (int)mat->stencil_fail_op;
    mr->stencil_zfail_op  = (int)mat->stencil_zfail_op;
    mr->stencil_pass_op   = (int)mat->stencil_pass_op;
    /* Guarded, unlike the stencil block above it.  Without the guard a scene
     * that authored a clearcoat on ONE instance of a shared material had it
     * overwritten with the material file's zero on the very next load -- and
     * silently, with the authored 1.0 still sitting in the .scene.json.
     * Measured before the bit existed: authored 1.000 at parse, 0.000 at
     * material registration, three probes apart. */
    if (!(ovr & JCE_MR_OVERRIDE_LOBES)) {
        mr->clearcoat           = mat->clearcoat;
        mr->clearcoat_roughness = mat->clearcoat_roughness;
        mr->anisotropy             = mat->anisotropy;
        mr->anisotropy_rotation    = mat->anisotropy_rotation;
        mr->translucency           = mat->translucency;
        mr->translucency_thickness = mat->translucency_thickness;
        mr->translucency_color[0]  = mat->translucency_color[0];
        mr->translucency_color[1]  = mat->translucency_color[1];
        mr->translucency_color[2]  = mat->translucency_color[2];
        mr->sheen_color[0]      = mat->sheen_color[0];
        mr->sheen_color[1]      = mat->sheen_color[1];
        mr->sheen_color[2]      = mat->sheen_color[2];
        mr->sheen_roughness     = mat->sheen_roughness;
    }

    if (!(ovr & JCE_MR_OVERRIDE_METALLIC))
        mr->metallic = mat->metallic_factor;
    if (!(ovr & JCE_MR_OVERRIDE_ROUGHNESS))
        mr->roughness = mat->roughness_factor;
    if (!(ovr & JCE_MR_OVERRIDE_EMISSIVE)) {
        mr->emissive[0] = mat->emissive_factor[0];
        mr->emissive[1] = mat->emissive_factor[1];
        mr->emissive[2] = mat->emissive_factor[2];
    }
    if (!(ovr & JCE_MR_OVERRIDE_NORMAL_SCALE))
        mr->normal_scale = mat->normal_scale;
    if (!(ovr & JCE_MR_OVERRIDE_AO_STRENGTH))
        mr->ao_strength = mat->ao_strength;
}

int jce_mesh_renderer_override_count(const JceMeshRenderer *mr)
{
    if (!mr) return 0;

    int n = 0;
    for (uint32_t b = 0; b < 6u; ++b)
        if (mr->material_override_mask & (1u << b)) n++;
    return n;
}
