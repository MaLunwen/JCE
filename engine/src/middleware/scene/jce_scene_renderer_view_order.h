/*
 * jce_scene_renderer_view_order.h  Pure bgfx view-order helper.
 */

#ifndef JCE_SCENE_RENDERER_VIEW_ORDER_H
#define JCE_SCENE_RENDERER_VIEW_ORDER_H

#include <jce/renderer/jce_views.h>

#include <stdbool.h>
#include <stdint.h>

/* 64 (was 32): omnidirectional point shadows (#7) add a sparse cube-tile view
 * band (base+100..) on top of the contiguous color/shadow/fog range. */
/* Big enough for a whole remap WINDOW, not just the ids the builder names:
 * the array must be a permutation of [first, highest], so the point-light cube
 * band (base+100..116) alone needs 117 entries. */
#define JCE_SCENE_RENDERER_VIEW_ORDER_MAX 128u

/* Dual shadow-map DYNAMIC atlas band: base+120 (full-atlas depth clear) and
 * base+121..124 (one tile per cascade).  Declared HERE, in the header both the
 * order builder and the shadow pass already include, so the band has exactly
 * one definition — two copies of a view-id constant is how the previous one
 * drifted.
 *
 * 120 puts it in the free 72..231 window, above the cube-shadow band
 * (base+100..116).  The previous band, base+21..25, was documented in-source as
 * free because it sits above the fog composite at base+16.  It is not: postfx
 * re-bases to base+20 (jce_editor_scene_render.cpp) and spans ~21 views, so
 * those five ids WERE PostFX/TAA_Resolve, TAA_HistoryCopy and Composite.  bgfx
 * view state is last-write-wins, so binding the atlas framebuffer to them
 * pointed the post-processing chain at the shadow atlas and the composited
 * image never reached the backbuffer -- the viewport rendered black whenever
 * dual shadow maps were enabled.  View ORDER cannot fix that: it decides when a
 * view runs, not which subsystem owns the slot. */
/* Offset of the dynamic-cascade shadow atlas from a viewport's view base.
 *
 * This is bounded by something easy to miss: bgfx_set_view_order rewrites the
 * whole contiguous window [first, first+count), so a viewport's order array
 * OWNS every id from its base up to the highest id it names.  Two viewports
 * therefore cannot each claim a window wider than the gap between their bases.
 *
 *   scene view  base = JCE_VIEW_EDITOR_SCENE =  3
 *   game view   base = GAME_VIEW_BASE        = 80      gap = 77
 *
 * At offset 120 the scene view's window was base..base+124 -- it swallowed the
 * whole game viewport and rewrote its sort keys, which is why enabling the
 * dynamic atlas rendered a black, flickering viewport.  Moving the band
 * elsewhere in that range does not help; the window is what overlaps.
 *
 * 52 keeps each window 57 ids wide, inside the 77 gap and disjoint:
 *
 *   scene view       base   3 ->  55..59   window   3..59
 *   game view        base  80 -> 132..136  window  80..136
 *   material preview base  60 -> 112..116  window  60..116
 *   runtime game     base  30 ->  82..86   window  30..86
 *
 * The editor's fixed ids that fall inside a window (EDITOR_OVERLAY 50,
 * EDITOR_PREVIEW 60, EDITOR_PICK 70/71) are fine: the builder appends every
 * unnamed id in the window so each keeps a sort position of its own.  What is
 * NOT fine is two viewports overlapping, which is what this constant guards.
 *
 * Anything that grows a viewport's claim past base+76 must move the bases
 * apart, not this number. */
/* Single-sourced from the public reservation table so the editor's
 * static_assert and this builder cannot drift apart. */
#define JCE_VIEW_DYN_CSM_OFFSET JCE_VIEW_SR_DYN_CSM_OFFSET

typedef struct {
    uint16_t first;
    uint16_t count;
    /* How many of `order` are views the scene renderer actually RENDERS INTO.
     * The rest are filler the builder appends to close the remap window into a
     * permutation (see the build function) -- ids that belong to postfx, the
     * editor overlays and whatever else lives in the span.  Sort position is
     * all we take from them; their view STATE belongs to their real owner.
     *
     * The distinction is not cosmetic: jce_view_bands_claim() reads this array
     * to declare ownership, and claiming the filler made the scene renderer
     * collide with postfx on every frame.  A guard that always fires cannot
     * report the collision it exists for. */
    uint16_t named_count;
    uint16_t order[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
} JceSceneRendererViewOrder;

bool jce_scene_renderer_view_order_build(uint16_t view_id_base,
                                         bool include_shadow_views,
                                         uint8_t csm_cascade_count,
                                         bool include_fog_views,
                                         bool include_gpu_particle_view,
                                         bool include_gpu_cull_view,
                                         bool include_point_cube_views,
                                         bool include_dyn_csm_views,
                                         /* Underwater absorption at base+17.
                                          * Gated so a scene with no absorbing
                                          * water keeps a minimal window. */
                                         bool include_underwater_view,
                                         uint8_t camera_overlay_count,
                                         JceSceneRendererViewOrder *out);

#endif /* JCE_SCENE_RENDERER_VIEW_ORDER_H */
