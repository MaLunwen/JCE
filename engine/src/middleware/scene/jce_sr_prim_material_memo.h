/*
 * jce_sr_prim_material_memo.h
 *
 * Pure primitive-instancing material signature. Base colour and mesh identity
 * are intentionally excluded: colour rides the per-instance tint stream and
 * mesh identity remains part of the draw-group key.
 */

#ifndef JCE_SR_PRIM_MATERIAL_MEMO_H
#define JCE_SR_PRIM_MATERIAL_MEMO_H

#include <jce/middleware/scene/jce_scene.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct JceSrPrimMaterialSignature {
    float    metallic;
    float    roughness;
    float    emissive[3];
    float    normal_scale;
    float    ao_strength;
    float    alpha_cutoff;
    int32_t  alpha_mode;
    uint16_t ssao_ao_idx;
    bool     double_sided;
    bool     shadow_receive_off;
    bool     ssao_active;
    /* func, ref, read mask, fail / zfail / pass op -- in that order.  The six
     * raw values rather than the assembled bgfx word: this header sees the
     * component, not a JcePbrMaterial, and re-deriving the word here would be
     * a second copy of the mapping that jce_pbr_material_stencil owns.
     * Splitting on an unused field (a ref value with the test off) costs one
     * extra material registration and no draw, which is the safe direction. */
    int32_t  stencil[6];
    /* clearcoat, clearcoat roughness, sheen rgb, sheen roughness -- in that
     * order.  Here for the same reason stencil[] is: the lobes upload as
     * uniforms in jce_pbr_material_bind, which an instanced run issues ONCE,
     * so two primitives differing only in a coat must not share a batch. */
    float    lobes[13];   /* [0..5] as above; APPENDED [6..12]:
                           * anisotropy, anisotropy rotation, translucency,
                           * translucency thickness, translucency rgb. */
    /* The RECEIVER'S RENDERING LAYER (0..31).  Not a material property, and
     * here for the same reason stencil[] and lobes[] are: an instanced run
     * issues ONE light upload for the whole batch, and which lights it
     * contains depends on the receiver's layer.  Measured: without it, a
     * second sphere on another layer hit this memo, took the first sphere's
     * material_key, and was lit as the first sphere's layer. */
    uint8_t  receiver_layer;
} JceSrPrimMaterialSignature;

static inline JceSrPrimMaterialSignature
jce_sr_prim_material_signature_make(const JceMeshRenderer *mr,
                                    bool ssao_active,
                                    uint16_t ssao_ao_idx,
                                    uint8_t receiver_layer)
{
    JceSrPrimMaterialSignature sig = {0};

    if (!mr)
        return sig;

    sig.metallic = mr->metallic;
    sig.roughness = mr->roughness;
    sig.emissive[0] = mr->emissive[0];
    sig.emissive[1] = mr->emissive[1];
    sig.emissive[2] = mr->emissive[2];
    sig.normal_scale = fabsf(mr->normal_scale);
    sig.ao_strength = mr->ao_strength;
    sig.alpha_cutoff = mr->alpha_cutoff;
    sig.alpha_mode = mr->alpha_mode;
    sig.double_sided = mr->double_sided;
    sig.shadow_receive_off = mr->shadow_receive_off;
    sig.ssao_active = ssao_active;
    sig.ssao_ao_idx = ssao_active ? ssao_ao_idx : UINT16_MAX;
    sig.stencil[0] = mr->stencil_func;
    sig.stencil[1] = mr->stencil_ref;
    sig.stencil[2] = mr->stencil_read_mask;
    sig.stencil[3] = mr->stencil_fail_op;
    sig.stencil[4] = mr->stencil_zfail_op;
    sig.stencil[5] = mr->stencil_pass_op;
    sig.lobes[0] = mr->clearcoat;
    sig.lobes[1] = mr->clearcoat_roughness;
    sig.lobes[2] = mr->sheen_color[0];
    sig.lobes[3] = mr->sheen_color[1];
    sig.lobes[4] = mr->sheen_color[2];
    sig.lobes[5] = mr->sheen_roughness;
    sig.lobes[6]  = mr->anisotropy;
    sig.lobes[7]  = mr->anisotropy_rotation;
    sig.lobes[8]  = mr->translucency;
    sig.lobes[9]  = mr->translucency_thickness;
    sig.lobes[10] = mr->translucency_color[0];
    sig.lobes[11] = mr->translucency_color[1];
    sig.lobes[12] = mr->translucency_color[2];
    sig.receiver_layer = (uint8_t)(receiver_layer & 31u);
    return sig;
}

static inline bool
jce_sr_prim_material_signature_equal(
    const JceSrPrimMaterialSignature *a,
    const JceSrPrimMaterialSignature *b)
{
    if (!a || !b)
        return false;

    return a->metallic == b->metallic &&
           a->roughness == b->roughness &&
           a->emissive[0] == b->emissive[0] &&
           a->emissive[1] == b->emissive[1] &&
           a->emissive[2] == b->emissive[2] &&
           a->normal_scale == b->normal_scale &&
           a->ao_strength == b->ao_strength &&
           a->alpha_cutoff == b->alpha_cutoff &&
           a->alpha_mode == b->alpha_mode &&
           a->ssao_ao_idx == b->ssao_ao_idx &&
           a->double_sided == b->double_sided &&
           a->shadow_receive_off == b->shadow_receive_off &&
           a->ssao_active == b->ssao_active &&
           a->stencil[0] == b->stencil[0] &&
           a->stencil[1] == b->stencil[1] &&
           a->stencil[2] == b->stencil[2] &&
           a->stencil[3] == b->stencil[3] &&
           a->stencil[4] == b->stencil[4] &&
           a->stencil[5] == b->stencil[5] &&
           a->lobes[0] == b->lobes[0] &&
           a->lobes[1] == b->lobes[1] &&
           a->lobes[2] == b->lobes[2] &&
           a->lobes[3] == b->lobes[3] &&
           a->lobes[4] == b->lobes[4] &&
           a->lobes[5] == b->lobes[5] &&
           a->receiver_layer == b->receiver_layer;
}

#endif /* JCE_SR_PRIM_MATERIAL_MEMO_H */
