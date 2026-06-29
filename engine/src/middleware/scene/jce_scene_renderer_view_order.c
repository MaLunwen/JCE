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
                                         bool include_point_cube_views,
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
       (no draws) that must execute before any consumer reads their output.
       Sharing one slot keeps the view range compact; ordering among dispatches
       on the same view is submission order, and they write disjoint buffers. */
    if (include_gpu_particle_view || include_gpu_cull_view) {
        uint16_t gp_view = (uint16_t)(view_id_base + 9u);
        if (gp_view > max_view)
            max_view = gp_view;
    }
    /* GPU-driven cull counter-RESET view (base+3): a SEPARATE earlier compute
       view so bgfx inserts a cross-view barrier between the reset and the
       compact atomics on base+9 (D3D12 needs this; same-view dispatches keep the
       counter in UAV state and get no barrier).  base+3 < base+9, already in
       [base, max_view] — only affects ordering, not the range. */

    uint32_t range_count = (uint32_t)max_view - (uint32_t)view_id_base + 1u;
    if (range_count > JCE_SCENE_RENDERER_VIEW_ORDER_MAX)
        return false;

    /* GPU-driven CULL views FIRST — before the shadow producer views — but ONLY
       when the GPU-cull path is live (include_gpu_cull_view).  The color cull
       feeds base+0 (color, pushed last), but the per-cascade SHADOW cull (roadmap
       #18 shadow extension) feeds the CSM cascade DRAW views base+11+c, which the
       shadow block below pushes EARLY.  So the cull/compact (base+9) and its
       counter-reset (base+3) must precede the shadow block, or a cascade's depth
       draw would run before its cull filled the visible/indirect buffers (garbage
       shadows).  base+3 before base+9 keeps the D3D12 cross-view barrier (reset's
       zero precedes compact's atomics).  When the GPU-cull path is OFF this block
       is skipped entirely and the order below is byte-identical to before (the
       particle compute view, if any, keeps its original post-shadow position). */
    if (include_gpu_cull_view) {
        if (!order_push(out, (uint16_t)(view_id_base + 3u)))
            return false;
        if (!order_push(out, (uint16_t)(view_id_base + 9u)))
            return false;
    }

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

    /* #7 omnidirectional point shadows: when on, ALL local-shadow tiles relocate
       to a FREE high view band base+100..base+116 (1 full-atlas clear + up to 16
       tiles: 4 spot/legacy + 2*6 cube faces) — base+50/60/70 are editor
       overlay/preview/pick, so the legacy base+4..8 band cannot grow there.
       Pushed here (before base+0) so the atlas is produced before the color view
       samples it. This band is SPARSE (outside [base, max_view]) so it adds to the
       order count beyond range_count (see the return below). Keep in sync with
       jce_sr_internal.h: JCE_VIEW_LOCAL_SHADOW_CUBE_OFFSET=100 + the 16-slot
       atlas budget (JCE_MAX_LOCAL_SHADOWS + JCE_POINT_SHADOW_MAX*6). */
    if (include_point_cube_views) {
        for (uint16_t v = (uint16_t)(view_id_base + 100u);
             v <= (uint16_t)(view_id_base + 116u); v++) {
            if (!order_push(out, v))
                return false;
        }
    }

    /* GPU particle compute view (base+9): the simulate/emit dispatches must
       execute before base+0 (the color view that draws the pool).  In the
       GPU-cull path base+9 was already pushed early above (order_push dedups, so
       this is a no-op then); in the particle-only path it lands HERE — its
       original position, just before base+0 — so that path's order is unchanged. */
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

    /* The cube-tile band (17 sparse views: base+100..+116) is outside
       [base, max_view], so it adds to the count beyond the contiguous range. */
    uint16_t expected = (uint16_t)range_count;
    if (include_point_cube_views) expected = (uint16_t)(expected + 17u);
    return out->count == expected;
}
