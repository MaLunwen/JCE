/*
 * jce_scene_renderer_view_order.c  Pure bgfx view-order helper.
 */

#include "jce_scene_renderer_view_order.h"

#include <jce/renderer/jce_csm.h>

#include <string.h>

static bool order_contains(const JceSceneRendererViewOrder *out,
                           uint16_t view_id)
{
    for (uint16_t i = 0; i < out->count; i++) {
        if (out->order[i] == view_id)
            return true;
    }
    return false;
}

static bool order_push(JceSceneRendererViewOrder *out, uint16_t view_id)
{
    if (out->count >= JCE_SCENE_RENDERER_VIEW_ORDER_MAX)
        return false;
    if (order_contains(out, view_id))
        return true;
    out->order[out->count++] = view_id;
    return true;
}

bool jce_scene_renderer_view_order_build(uint16_t view_id_base,
                                         bool include_shadow_views,
                                         uint8_t csm_cascade_count,
                                         bool include_fog_views,
                                         bool include_gpu_particle_view,
                                         bool include_gpu_cull_view,
                                         JceSceneRendererViewOrder *out)
{
    if (!out)
        return false;

    memset(out, 0, sizeof(*out));
    out->first = view_id_base;

    if (csm_cascade_count > JCE_CSM_MAX_CASCADES)
        csm_cascade_count = JCE_CSM_MAX_CASCADES;

    uint16_t max_view = view_id_base;
    if (include_shadow_views) {
        uint16_t shadow_max = (uint16_t)(view_id_base + 10u);
        if (csm_cascade_count > 0)
            shadow_max = (uint16_t)(view_id_base + 10u + csm_cascade_count);
        if (shadow_max > max_view)
            max_view = shadow_max;
    }
    if (include_fog_views) {
        uint16_t fog_composite_view = (uint16_t)(view_id_base + 16u);
        if (fog_composite_view > max_view)
            max_view = fog_composite_view;
    }
    /* The GPU-driven cull dispatch (roadmap #18) shares the pre-color compute
       band slot base+9 with the GPU particle compute view — both are dispatches
       (no draws) that must execute before the color view consumes their output.
       Sharing one slot keeps the view range compact; ordering among dispatches
       on the same view is submission order, and the two write disjoint
       buffers. */
    if (include_gpu_particle_view || include_gpu_cull_view) {
        uint16_t gp_view = (uint16_t)(view_id_base + 9u);
        if (gp_view > max_view)
            max_view = gp_view;
    }

    uint32_t range_count = (uint32_t)max_view - (uint32_t)view_id_base + 1u;
    if (range_count > JCE_SCENE_RENDERER_VIEW_ORDER_MAX)
        return false;

    if (include_shadow_views) {
        if (!order_push(out, (uint16_t)(view_id_base + 10u)))
            return false;
        for (uint8_t c = 0; c < csm_cascade_count; c++) {
            if (!order_push(out, (uint16_t)(view_id_base + 11u + c)))
                return false;
        }
        /* P1 — local (spot/point) shadow atlas views: 1 full-atlas depth
           clear (base+4) + up to 4 tiles (base+5..base+8), all packed in the
           free band below the shadow band. Pushed before the color view so
           the atlas is produced before it is sampled. They sit within
           [base, max_view] so the dedup keeps out->count == range_count. */
        for (uint16_t v = (uint16_t)(view_id_base + 4u);
             v <= (uint16_t)(view_id_base + 8u); v++) {
            if (!order_push(out, v))
                return false;
        }
    }

    /* GPU particle compute view (base+9): the simulate/emit dispatches must
       execute before the color view that draws the pool, so it is pushed
       ahead of base+0.  Sits within [base, max_view]; the dedup keeps
       out->count == range_count. */
    if (include_gpu_particle_view || include_gpu_cull_view) {
        if (!order_push(out, (uint16_t)(view_id_base + 9u)))
            return false;
    }

    if (!order_push(out, view_id_base))
        return false;

    for (uint16_t view_id = (uint16_t)(view_id_base + 1u);
         view_id <= max_view; view_id++) {
        if (!order_push(out, view_id))
            return false;
    }

    return out->count == (uint16_t)range_count;
}
