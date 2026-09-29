/*
 * jce_scene_renderer_view_order.c  Pure bgfx view-order helper.
 */

#include "jce_scene_renderer_view_order.h"

#include <jce/renderer/jce_csm.h>

#include <string.h>

/* -- PROOF: every LIVE band is inside the FROZEN set -------------------
 *
 * The frozen set (jce_views.h) is what the editor's viewport offsets are
 * static_asserted against, and it is deliberately NOT derived from the
 * constants below -- a predicate built from the same constants the builder
 * uses cannot disagree with it, and it stops reserving an id the moment a band
 * vacates it.
 *
 * The cost of that split is that the two CAN drift, in one direction: a live
 * band could move outside the frozen union while the editor's asserts kept
 * testing the old union.  These catch exactly that, at compile time, in
 * TRACKED engine source -- not in tests/, which has zero tracked files on this
 * branch and therefore cannot be cited as a gate.
 *
 * Moving a band is legal.  Moving it outside the frozen set without appending
 * its new home to that set is not. */
#define JCE_VO_SASSERT(cond, tag) typedef char jce_vo_sa_##tag[(cond) ? 1 : -1]

JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_CORE_SPAN - 1u),
               core_span_top_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_SSAO_OFFSET),
               ssao_sample_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_SSAO_OFFSET +
                                           JCE_VIEW_SR_SSAO_COUNT - 1u),
               ssao_blur_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_GPU_CULL_RESET_OFFSET),
               gpu_cull_reset_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_DYN_CSM_OFFSET),
               dyn_csm_lo_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_DYN_CSM_OFFSET +
                                           JCE_VIEW_SR_DYN_CSM_COUNT - 1u),
               dyn_csm_hi_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_POINT_CUBE_OFFSET),
               point_cube_lo_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_POINT_CUBE_OFFSET +
                                           JCE_VIEW_SR_POINT_CUBE_COUNT - 1u),
               point_cube_hi_is_frozen);
/* SSGI's pair and the camera-overlay band.  These two asserts are why the
 * predicate's missing clauses surfaced: JCE_VIEW_SR_SSGI_OFFSET had no line
 * here, so nothing ever asked JCE_VIEW_SR_OFFSET_RESERVED about it. */
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_SSGI_OFFSET),
               ssgi_march_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_SSGI_COMPOSITE_OFFSET),
               ssgi_composite_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET),
               camera_overlay_lo_is_frozen);
JCE_VO_SASSERT(JCE_VIEW_SR_OFFSET_RESERVED(JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET +
                                           JCE_VIEW_SR_CAMERA_OVERLAY_MAX - 1u),
               camera_overlay_hi_is_frozen);

/* And the frozen set may only ever GROW.  A narrowed range hands ids the
 * renderer once named back to whoever asks for them first, which is how the
 * original defect happened.  Pinned as literals on purpose: a check written in
 * terms of the thing it checks is not a check. */
JCE_VO_SASSERT(JCE_VIEW_SR_FROZEN_0_LO ==   0u && JCE_VIEW_SR_FROZEN_0_HI >=  17u,
               frozen_range_0_never_narrows);
JCE_VO_SASSERT(JCE_VIEW_SR_FROZEN_1_LO <=  52u && JCE_VIEW_SR_FROZEN_1_HI >=  57u,
               frozen_range_1_never_narrows);
JCE_VO_SASSERT(JCE_VIEW_SR_FROZEN_2_LO <= 100u && JCE_VIEW_SR_FROZEN_2_HI >= 116u,
               frozen_range_2_never_narrows);
JCE_VO_SASSERT(JCE_VIEW_SR_FROZEN_3_LO <=  63u && JCE_VIEW_SR_FROZEN_3_HI >=  63u,
               frozen_range_3_never_narrows);
JCE_VO_SASSERT(JCE_VIEW_SR_FROZEN_4_LO <=  26u && JCE_VIEW_SR_FROZEN_4_HI >=  27u,
               frozen_range_4_never_narrows);
JCE_VO_SASSERT(JCE_VIEW_SR_FROZEN_5_LO <=  64u && JCE_VIEW_SR_FROZEN_5_HI >=  66u,
               frozen_range_5_never_narrows);

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
                                         bool include_dyn_csm_views,
                                         bool include_underwater_view,
                                         uint8_t camera_overlay_count,
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
    /* Underwater absorption (base+17).  Gated on the scene CONTAINING water
     * with absorption authored, not on the camera being submerged: submersion
     * is decided during the water draw, which happens after this range is
     * built, so gating on it would leave the pass outside the ordered range on
     * exactly the frame you enter the water. */
    if (include_underwater_view) {
        uint16_t uw_view = (uint16_t)(view_id_base + 17u);
        if (uw_view > max_view)
            max_view = uw_view;
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
        if (!order_push(out, (uint16_t)(view_id_base +
                                        JCE_VIEW_SR_GPU_CULL_RESET_OFFSET)))
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
        for (uint16_t v = (uint16_t)(view_id_base +
                                     JCE_VIEW_SR_POINT_CUBE_OFFSET);
             v <= (uint16_t)(view_id_base +
                             JCE_VIEW_SR_POINT_CUBE_OFFSET +
                             JCE_VIEW_SR_POINT_CUBE_COUNT - 1u); v++) {
            if (!order_push(out, v))
                return false;
        }
    }

    /* Dual shadow-map DYNAMIC atlas: 1 full-atlas depth clear + 4 cascade tiles,
       pushed BEFORE the color view so the atlas is produced before the PBR pass
       samples it (via the s_shadowMap stage).  A SPARSE band (outside
       [base, max_view]) so it adds 5 to the order count beyond range_count (see
       the return below).

       Lives at base+JCE_VIEW_DYN_CSM_OFFSET (52) .. +56, clear of both the
       postfx range (base+20..40) and the cube band
       and above it.  It was base+21..25, which is NOT free: postfx re-bases to
       base+20, so those five ids were PostFX/TAA_Resolve, TAA_HistoryCopy and
       Composite.  Last-write-wins view state pointed the postfx chain at the
       shadow atlas and the frame never reached the backbuffer.  Keep in sync
       with JCE_VIEW_DYN_CSM_OFFSET in jce_sr_internal.h. */
    if (include_dyn_csm_views) {
        for (uint16_t v = (uint16_t)(view_id_base + JCE_VIEW_DYN_CSM_OFFSET);
             v <= (uint16_t)(view_id_base + JCE_VIEW_DYN_CSM_OFFSET +
                             JCE_VIEW_SR_DYN_CSM_COUNT - 1u); v++) {
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

    /* CAMERA-STACK OVERLAYS.  AFTER the base camera's whole contiguous run,
     * and before everything that READS the colour it produced.
     *
     * After the whole run, not merely after the colour view, because base+16
     * and base+17 -- the volumetric fog composite and the underwater
     * absorption -- DRAW INTO that colour.  An overlay ordered ahead of them
     * would be fogged and tinted by the base camera's atmosphere, which is
     * the opposite of what a stack is for: a first-person weapon does not get
     * the level's fog applied to it twice, and Unity gives an overlay its own
     * volume stack for the same reason.
     *
     * And they cannot go in the filler tail, which is ASCENDING ID ORDER:
     * base+64 would sort after SSR (base+18), after the project fullscreen
     * chain (base+20..25), after SSGI's composite (base+27) and after the
     * PostFX chain (base+30), compositing the overlay onto a tone-mapped
     * image.  A SPARSE band, so it adds to the count beyond range_count and
     * the window closure below covers it. */
    if (camera_overlay_count > JCE_VIEW_SR_CAMERA_OVERLAY_MAX)
        camera_overlay_count = (uint8_t)JCE_VIEW_SR_CAMERA_OVERLAY_MAX;
    for (uint8_t o = 0; o < camera_overlay_count; o++) {
        if (!order_push(out, (uint16_t)(view_id_base +
                                        JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET + o)))
            return false;
    }

    /* ── Close the window into a true permutation ─────────────────────────
     *
     * bgfx_set_view_order(first, count, order) does
     *
     *     memCopy(&m_viewRemap[first], order, count * sizeof(ViewId))
     *
     * and later inverts it: viewRemap[m_viewRemap[ii]] = ii, so order[i] is
     * "the view that sorts at position first+i".  The values may name views
     * anywhere, but the WINDOW it overwrites is always the contiguous run
     * [first, first + count).
     *
     * The sparse bands (cube tiles at base+100..116, dynamic CSM atlas at
     * base+52..56) each push ids from outside [base, max_view], so count
     * grew past the contiguous range while `first` stayed at base.  Every id
     * in the overshoot -- base+range_count .. base+count-1 -- had its sort key
     * silently rewritten to whatever sat at the tail of the array, and those
     * are live views: the scene viewport's own upscale/present target is at
     * base+POST_BASE+23, inside the overshoot.  Two views then claim one sort
     * position and the composite can run before the pass that fills it, which
     * is why enabling the dynamic atlas rendered a black, flickering viewport
     * wherever a band was active.
     *
     * Fix: make the array cover its whole window.  Everything already pushed
     * keeps its position; every remaining id in [first, highest_pushed] is
     * appended so no live view is left holding a borrowed sort key.  The
     * appended ids are unused slots, so their relative order does not matter.
     *
     * The count is no longer a fixed arithmetic expectation -- it is
     * whatever the span requires -- so the check below verifies the property
     * that actually matters: the array is a permutation of its own window. */
    out->named_count = (uint16_t)out->count;   /* everything above is ours */

    uint16_t highest = out->first;
    for (uint16_t i = 0; i < out->count; i++)
        if (out->order[i] > highest) highest = out->order[i];

    for (uint16_t v = out->first; v <= highest; v++) {
        if (!order_push(out, v))
            return false;          /* window wider than the order buffer */
    }

    /* Permutation check: count must span first..highest exactly once each.
     * order_push dedups, so a correct build has one entry per id. */
    return out->count == (uint32_t)(highest - out->first + 1u);
}
