/*
 * jce_sr_material_key.c -- the batch identity of a material.
 *
 * Split out of jce_scene_renderer.c (6626 lines) because it is one rule with
 * one job: decide whether two draws may share a submit.  Everything the
 * SUBMIT carries has to be in it -- the textures and factors the bind
 * uploads, and the alpha state, blend equation and stencil word that reach
 * the GPU through set_state / set_stencil.  A field that is missing here does
 * not fail; it makes a batch apply the first entry's value to the rest, which
 * is the shape of a feature that works on one object and lies about the rest.
 */
#include "jce_sr_internal.h"

/* ── Material registry (Phase 2) ──────────────────────────────────────
 * Per-frame registry mapping a material_key → texture/uniform snapshot.
 * Built during scene_renderer's mesh walk, consumed by sr_bind_material_cb
 * once jce_render_queue starts a new material run. Wired into the queue
 * in Phase 3. */

uint32_t sr_compute_material_key(const JcePbrMaterial *pbr,
                                        bool is_terrain,
                                        int terrain_slot,
                                        const bgfx_texture_handle_t *terrain_layer_tex,
                                        uint32_t receiver_layer)
{
    uint32_t h = JCE_FNV1A32_INIT;
    /* Texture handles (idx is enough — invalid = UINT16_MAX). */
    h = jce_fnv1a32_append(h, &pbr->albedo_map.idx,             sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->metallic_roughness_map.idx, sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->normal_map.idx,             sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->ao_map.idx,                 sizeof(uint16_t));
    h = jce_fnv1a32_append(h, &pbr->emissive_map.idx,           sizeof(uint16_t));
    /* Factors. */
    h = jce_fnv1a32_append(h, pbr->base_color_factor, sizeof(pbr->base_color_factor));
    h = jce_fnv1a32_append(h, &pbr->metallic_factor,  sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->roughness_factor, sizeof(float));
    h = jce_fnv1a32_append(h, pbr->emissive_factor,   sizeof(pbr->emissive_factor));
    h = jce_fnv1a32_append(h, &pbr->normal_scale,     sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->ao_strength,      sizeof(float));
    /* State. */
    uint32_t am = (uint32_t)pbr->alpha_mode;
    h = jce_fnv1a32_append(h, &am,                  sizeof(am));
    h = jce_fnv1a32_append(h, &pbr->alpha_cutoff,   sizeof(float));
    uint8_t ds = pbr->double_sided ? 1u : 0u;
    h = jce_fnv1a32_append(h, &ds,                  sizeof(ds));
    /* Blend equation and stencil word: both reach the GPU through the SUBMIT
     * (set_state / set_stencil), and a batch issues one of each for its whole
     * run -- so two materials differing only here must not collapse into one
     * draw.  ASSEMBLED words, not raw fields: that is what the submit
     * compares, so an unused field (a ref value with the test off) still
     * shares its batch. */
    uint32_t bm = (uint32_t)pbr->blend_mode;
    h = jce_fnv1a32_append(h, &bm,                  sizeof(bm));
    uint32_t sc = jce_pbr_material_stencil(pbr);
    h = jce_fnv1a32_append(h, &sc,                  sizeof(sc));
    /* Extended lobes.  They upload in jce_pbr_material_bind, which a batch
     * issues ONCE for the whole run -- so two materials differing only in a
     * clearcoat must not collapse into one draw and take the first one's. */
    h = jce_fnv1a32_append(h, &pbr->clearcoat,           sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->clearcoat_roughness, sizeof(float));
    h = jce_fnv1a32_append(h, pbr->sheen_color,          sizeof(pbr->sheen_color));
    h = jce_fnv1a32_append(h, &pbr->sheen_roughness,     sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->anisotropy,             sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->anisotropy_rotation,    sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->translucency,           sizeof(float));
    h = jce_fnv1a32_append(h, &pbr->translucency_thickness, sizeof(float));
    h = jce_fnv1a32_append(h, pbr->translucency_color,
                           sizeof(pbr->translucency_color));
    /* Terrain pseudo-fields (slot ensures distinct splat/layer textures). */
    uint8_t it = is_terrain ? 1u : 0u;
    h = jce_fnv1a32_append(h, &it, sizeof(it));
    int32_t ts = (int32_t)terrain_slot;
    h = jce_fnv1a32_append(h, &ts, sizeof(ts));
    /* Terrain layer texture handles also folded in so two terrain entities
     * with the same slot but different runtime layer textures still split
     * into separate batches (rare today, but keeps key correctness). */
    if (terrain_layer_tex) {
        for (int li = 0; li < 4; li++)
            h = jce_fnv1a32_append(h, &terrain_layer_tex[li].idx, sizeof(uint16_t));
    }
    /* The RECEIVER'S RENDERING LAYER (0..31).  Not a property of the material
     * at all -- it is in the key because a batch uploads ONE light set for its
     * whole run, and a light's layer mask is evaluated against the receiver.
     * Two objects on different layers therefore cannot share a submit even
     * when every material field above matches, which is the general rule this
     * file states: everything the SUBMIT carries has to be in here.
     *
     * Folded unconditionally rather than only when some light carries a mask.
     * A frame-dependent key would be cached across frames by the draw-command
     * cache and then read back after the condition flipped.  A scene where
     * every object sits on layer 0 -- which is every scene that never opened
     * the layer editor -- appends the same byte to every key and batches
     * exactly as it did before. */
    uint8_t rl = (uint8_t)(receiver_layer & 31u);
    h = jce_fnv1a32_append(h, &rl, sizeof(rl));
    return h ? h : 1u;
}
