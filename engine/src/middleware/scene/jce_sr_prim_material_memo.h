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
} JceSrPrimMaterialSignature;

static inline JceSrPrimMaterialSignature
jce_sr_prim_material_signature_make(const JceMeshRenderer *mr,
                                    bool ssao_active,
                                    uint16_t ssao_ao_idx)
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
           a->ssao_active == b->ssao_active;
}

#endif /* JCE_SR_PRIM_MATERIAL_MEMO_H */
