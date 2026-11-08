/* Private colour-call staging over the existing entity/cull arrays.
 * Capture dispatch membership once; no queue, allocation or second admission. */
#ifndef JCE_SR_COLOUR_SPECIAL_INTERNAL_H
#define JCE_SR_COLOUR_SPECIAL_INTERNAL_H

#include "middleware/scene/jce_sr_grass_mask.h"

enum {
    SR_COLOUR_SCATTER = 1u, SR_COLOUR_CLUSTER = 2u, SR_COLOUR_GRASS = 4u,
    SR_COLOUR_LINE = 8u, SR_COLOUR_TRAIL = 16u, SR_COLOUR_WATER = 32u,
    SR_COLOUR_OPAQUE = SR_COLOUR_SCATTER | SR_COLOUR_CLUSTER | SR_COLOUR_GRASS,
    SR_COLOUR_TRANSPARENT = SR_COLOUR_LINE | SR_COLOUR_TRAIL | SR_COLOUR_WATER
};
typedef struct { bool opaque, transparent; } SrColourSpecialPass;

static void sr_colour_special_defer(SrColourSpecialPass *pass, uint8_t *flags, uint8_t kind)
{
    *flags |= kind;
    if (kind & SR_COLOUR_OPAQUE) pass->opaque = true;
    if (kind & SR_COLOUR_TRANSPARENT) pass->transparent = true;
}

/* The caller has flushed the ordinary opaque models and render queue. Keep
 * original entity order and each producer's world/cell/material/shader policy.
 * Per-entity qualification and priority were already captured by the main loop. */
static void sr_colour_special_submit_opaque(JceSceneRenderer *sr, JceScene *scene,
    const JceCamera *camera, EntityList *list, uint16_t view_id,
    const JceSceneRenderConfig *cfg, const SrColourSpecialPass *pass,
    SrGrassGatePass *grass_gate_pass_ptr)
{
    if (!pass->opaque) return;
    SrGrassMaskBatch mask_batch = {0};
    for (int i = 0; i < list->count; ++i) {
        const uint8_t flags = sr->ecull[i].colour_special;
        if (!(flags & SR_COLOUR_OPAQUE)) continue;
        sr->ecull[i].colour_special = (uint8_t)(flags & ~SR_COLOUR_OPAQUE);
        const JceEntity e = list->entities[i];
        sr->model_receiver_layer = sr->ecull[i].layer;
        if (flags & SR_COLOUR_SCATTER) {
            const int measured = jce_perf_phase_enabled();
            const uint64_t begin = measured ? jce_time_perf_counter() : 0u;
            sr_draw_foliage(sr, scene, list, e, view_id, camera);
            if (measured) jce_perf_phase_add("sr_foliage",
                jce_time_perf_to_ms(begin, jce_time_perf_counter()));
        } else if (flags & SR_COLOUR_CLUSTER) {
            sr_draw_foliage_cluster(sr, scene, list, e, view_id, camera);
        } else if (flags & SR_COLOUR_GRASS) {
            sr_grass_draw_colour_request(sr, scene, list, e, view_id, camera,
                cfg, grass_gate_pass_ptr, &mask_batch);
        }
    }
    sr_grass_mask_batch_end(&mask_batch);
}

/* Custom blended producers retain their original per-entity Line/Trail/Water
 * order, after the model/vegetation stages. This is not a global depth sort. */
static void sr_colour_special_submit_transparent(JceSceneRenderer *sr, JceScene *scene,
    const JceCamera *camera, EntityList *list, uint16_t view_id,
    const SrColourSpecialPass *pass)
{
    if (!pass->transparent) return;
    uint32_t water_cpu_view_vertices = 0u;
    for (int i = 0; i < list->count; ++i) {
        const uint8_t flags = sr->ecull[i].colour_special;
        if (!(flags & SR_COLOUR_TRANSPARENT)) continue;
        sr->ecull[i].colour_special = (uint8_t)(flags & ~SR_COLOUR_TRANSPARENT);
        const JceEntity e = list->entities[i];
        sr->model_receiver_layer = sr->ecull[i].layer;
        if (flags & SR_COLOUR_LINE)
            sr_draw_line_renderer(sr, scene, list, e, camera, view_id);
        if (flags & SR_COLOUR_TRAIL)
            sr_draw_trail_renderer(sr, scene, list, e, camera, view_id);
        if (flags & SR_COLOUR_WATER)
            sr_draw_water(sr, scene, camera, list, e, view_id,
                          &water_cpu_view_vertices);
    }
}
#endif
