/*
 * jce_editor_viewport_common.h  Shared per-viewport render plumbing.
 *
 * The Scene View (jce_editor_scene_render.cpp) and the Game View
 * (jce_editor_game_render.cpp) stay two SEPARATE viewports: different cameras,
 * different overlays, different lifetimes.  What they must not differ in is the
 * state they hand the engine every frame — the offscreen-bridge wiring the
 * renderer reads out of JceSceneRenderConfig, the fixed view-id offsets of the
 * post passes, and (for the Game View's second PostFX pipeline) the PostFX look
 * itself.  Each of those was spelled out twice, and every copy is a place a new
 * field can be added to one viewport and forgotten in the other — the SSR/GI
 * lesson already recorded in the Game View: every cfg-dependent effect must be
 * wired on BOTH editor render paths.
 *
 * Those conversions live here once.  This header owns no state and merges no
 * viewport: anything genuinely per-viewport (view mode, camera, cull focus,
 * overlays, TAA history, the Scene View's TSR/FXAA override) stays at the call
 * site, where it reads as the deliberate difference it is.
 *
 * Include OUTSIDE any extern "C" block — the helpers are C++ inline functions.
 */

#ifndef JCE_EDITOR_VIEWPORT_COMMON_H
#define JCE_EDITOR_VIEWPORT_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_views.h>
#include <jce/renderer/jce_volumetric_fog.h>

/* Lighting panel accessors — defined in jce_panel_lighting_settings.cpp.
 * Declared once here instead of in each viewport translation unit. */
bool jce_editor_lighting_get_ambient(float out_color_rgb[3], float *out_intensity);
}

#include "core/jce_editor_config.h"

/* ── PostFX state ─────────────────────────────────────────────────── */

/* True when any built-in effect slot is enabled.  Both viewports gate their
 * postfx chain on this. */
inline bool jce_editor_viewport_postfx_any_effect(const JcePostFXPipeline *pfx)
{
    if (!pfx) return false;
    for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
        if (jce_postfx_is_enabled(pfx, (JcePostFXType)i))
            return true;
    }
    return false;
}

/* Replay the AUTHORABLE PostFX state of `src` onto `dst`.
 *
 * The Scene View renders through the engine scene renderer's own pipeline; the
 * Game View owns a SECOND pipeline (its own bgfx view ids) and therefore has to
 * replay that state so PostFX panel edits show up in both.  This is the only
 * place that replay exists — a new authorable PostFX field belongs in this one
 * function, never in a viewport.
 *
 * Deliberately NOT copied (per-pipeline, not per-look; the owning viewport
 * drives each around its own camera and resolution):
 *   view base  — jce_postfx_set_view_base, fixed at init and must differ
 *   chain size — jce_postfx_resize, each viewport's own render resolution
 *   TAA        — jce_postfx_set_taa / _taa_matrices / _taa_motion_tex
 */
inline void jce_editor_viewport_postfx_mirror(JcePostFXPipeline       *dst,
                                              const JcePostFXPipeline *src)
{
    if (!dst || !src) return;

    for (int i = 0; i < JCE_POSTFX_COUNT; i++)
        jce_postfx_enable(dst, (JcePostFXType)i,
                          jce_postfx_is_enabled(src, (JcePostFXType)i));

    JcePostFXParams params;
    jce_postfx_get_params(src, &params);
    jce_postfx_set_params(dst, &params);

    /* Data-driven custom pass (shader name + generic vec4 params). */
    char  cust_name[64] = {0};
    bool  cust_depth    = false;
    float cust_params[JCE_POSTFX_CUSTOM_PARAMS * 4];
    jce_postfx_get_custom_shader(src, cust_name, (int)sizeof(cust_name),
                                 &cust_depth);
    int cust_count = jce_postfx_get_custom_params(src, cust_params,
                                                  JCE_POSTFX_CUSTOM_PARAMS);
    jce_postfx_set_custom_shader(dst, cust_name, cust_depth);
    jce_postfx_set_custom_params(dst, cust_params, cust_count);

    /* Postfx finish: tonemap operator / bloom shape / colour-grade LUT. */
    jce_postfx_set_tonemap_op(dst, jce_postfx_get_tonemap_op(src));
    jce_postfx_set_bloom_knee(dst, jce_postfx_get_bloom_knee(src));
    jce_postfx_set_bloom_quality(dst, jce_postfx_get_bloom_quality(src));

    /* Auto exposure.  Mirrored like everything else here because the two
     * viewports render in the SAME frame: a gate set on one and not the other
     * shows the same scene at two different exposures side by side, and the
     * reflex is to call that a rendering bug. */
    {
        JceAutoExposureDesc ae;
        jce_postfx_get_auto_exposure_desc(src, &ae);
        jce_postfx_set_auto_exposure(dst, jce_postfx_get_auto_exposure(src), &ae);
    }

    JceTexture lut          = { UINT16_MAX };
    int        lut_size     = 0;
    float      lut_strength = 0.0f;
    jce_postfx_get_lut(src, &lut, &lut_size, &lut_strength);
    jce_postfx_set_lut(dst, lut, lut_size, lut_strength);  /* shares the 3D handle */
}

/* ── Per-frame render-config plumbing ─────────────────────────────── */

/* Wire the offscreen bridge and the real panel resolution into the engine
 * render config.  Every field set here must match between the two viewports or
 * one of them silently renders a different effect.
 *
 * The resolution matters beyond the colour pass: the SSAO/SSR offscreen targets
 * and the screen-space AO sampling UV are derived from it, so a viewport that
 * leaves it 0 falls back to 1920x1080 and the PBR shader samples AO at
 * misregistered UVs (a scaled, one-frame-late grey smear trailing moving
 * skinned geometry).
 *
 * Per-viewport fields — view_mode, viewport_id, on_after_sky, cull focus, the
 * occlusion culler instance — stay at the call site. */
inline void jce_editor_viewport_apply_shared_config(JceSceneRenderConfig *cfg,
                                                    JceOffscreenTarget   *bridge,
                                                    uint32_t              width,
                                                    uint32_t              height,
                                                    const JceScene       *scene)
{
    if (!cfg) return;

    /* Broadphase frustum culling — uniform-grid backed. Stats appear in the
     * Profiler panel under "Scene Culling". */
    cfg->frustum_culling = true;
    cfg->viewport_width  = width;
    cfg->viewport_height = height;

    /* Both viewports render into an offscreen bridge FBO — bind the occlusion
     * proxy view to it so its depth test reads the depth the colour pass wrote
     * (against the backbuffer the queries go inert or false-cull). */
    cfg->scene_frame_buffer = jce_offscreen_target_get_frame_buffer(bridge);

    /* Volumetric fog.  Derived by the ENGINE from the scene's authored
     * settings, not by an editor accessor: the editor-only mapping is why a
     * shipped game never ran this pass at all.  Both now call the same
     * function, so what the editor previews is what a build renders. */
    /* Scene depth is published unconditionally: it is the offscreen target's,
     * not fog's.  Gating it on fog_enabled -- which is what the old
     * fog_depth_tex_handle name invited -- meant that switching fog off also
     * switched off underwater absorption, which reads the same depth for an
     * unrelated reason.  Only fog's own RT sizing belongs inside the gate. */
    cfg->scene_depth_tex_handle = jce_offscreen_target_get_depth_texture(bridge);

    /* Both editor viewports composite (jce_editor_viewport_composite_fog_ssr,
     * called from jce_editor_scene_render.cpp and jce_editor_game_render.cpp),
     * so declare it here -- in the one helper both fill their config through --
     * rather than at the two call sites, where the two could drift apart and
     * one viewport would fog twice while the other did not. */
    cfg->composites_volumetric_fog = true;

    cfg->fog_enabled = jce_scene_fog_params_from_scene(scene, &cfg->fog);
    if (cfg->fog_enabled) {
        cfg->fog_rt_width  = (int)width;
        cfg->fog_rt_height = (int)height;
    } else {
        cfg->fog_rt_width  = 0;
        cfg->fog_rt_height = 0;
    }

    /* SSR reflects the bridge's lit colour RT (gated on the scene's
     * ssr_enabled); GI L1's dynamic probe gather samples the same RT. */
    cfg->ssr_color_tex_handle = jce_offscreen_target_get_color_texture(bridge);
    cfg->gi_color_tex_handle  = cfg->ssr_color_tex_handle;
}

/* Push the Lighting panel's ambient override onto the shared engine renderer.
 * Both viewports apply it so panel edits reach whichever renders. */
inline void jce_editor_viewport_apply_ambient_override(JceSceneRenderer *sr)
{
    if (!sr) return;
    float amb_color[3];
    float amb_intensity = 0.15f;
    if (jce_editor_lighting_get_ambient(amb_color, &amb_intensity))
        jce_scene_renderer_set_ambient_override(sr, amb_color, amb_intensity);
    else
        jce_scene_renderer_set_ambient_override(sr, nullptr, 0.0f);
}

/* ── Post passes over the bridge ──────────────────────────────────── */

/* ── Per-viewport bgfx view-id map ─────────────────────────────────────
 *
 * Every id here is BASE-RELATIVE.  The Scene View renders at
 * JCE_VIEW_EDITOR_SCENE, the Game View at JCE_EDITOR_GAME_VIEW_BASE, and each
 * adds these offsets to its own base.
 *
 * Two constraints bound the numbers, and the first one is not obvious:
 *
 *  1. An offset must NOT be one the scene renderer NAMES
 *     (JCE_VIEW_SR_OFFSET_RESERVED, <jce/renderer/jce_views.h>).  The renderer
 *     hands its named ids to bgfx_set_view_order() so the shadow/cull producer
 *     passes sort BEFORE the colour pass; the builder then appends every
 *     unnamed id in the window as filler, and all filler sorts AFTER all named
 *     ids.  So an editor pass on a FILLER id draws after the scene (fine),
 *     while an editor pass on a NAMED id inherits the producer's early sort
 *     position: it rasterises into the bridge and the colour pass's clear then
 *     erases it.  No error, no log, no pixels.
 *
 *     The tail block below used to be 52..56 -- byte for byte the dynamic-CSM
 *     atlas band (JCE_VIEW_SR_DYN_CSM_OFFSET .. +4).  That is why the ECS-UI
 *     Canvas overlay drew nothing in either editor viewport while the shipped
 *     runtime, which uses JCE_VIEW_UI (254, outside every remap window), drew
 *     it correctly.  Control: JCE_SHADOW_DUAL=0 removed the band from the order
 *     and the UI reappeared in both viewports, unchanged otherwise.
 *
 *     The static_asserts under the enum are that rule as a gate.  They are not
 *     decoration: this collision was introduced by renumbering these offsets
 *     while the reservation lived only in an engine-private comment.
 *
 *  2. base + the highest offset must stay below the next fixed id in
 *     <jce/renderer/jce_views.h> and below the other viewport's base.
 *
 * KNOWN, UNFIXED, OPT-IN: with omnidirectional point shadows on
 * (r.point_shadows / JCE_POINT_CUBE_SHADOWS, default OFF) the scene renderer
 * also names base+100..116.  For the Scene View that is 103..119, which lands
 * inside the GAME view's own range -- a cross-BASE conflict this header cannot
 * assert away, because the fix is to re-space the bases and the 256-id bgfx
 * budget has no room.  Documented here rather than left to be rediscovered.
 *
 * The TAIL passes (base+58..62) run after the scene colour pass purely because
 * they are outside the named set: bgfx keeps their identity sort key, which is
 * higher than any position the remap window hands out.  Their order among
 * themselves is their numeric order -- UI overlay, then the dynamic-resolution
 * resolve, then the read-back capture, then TSR. */
enum : uint16_t {
    /* Screen-space reflections composite (engine pass, editor-driven). */
    JCE_EDITOR_VP_SSR_COMPOSITE_OFFSET        = 19,
    /* Project-authored HDR full-screen stages, and their fold-back composite.
     * The stages run 20 .. 20+JCE_VIEW_SR_FULLSCREEN_MAX-1 = 20..25; 26 and 27
     * are SSGI's march and composite, which have to sit below the postfx base
     * at 30.  The engine enforces the cap (jce_sr_fullscreen_effect.c) and the
     * static_assert below is the other half: if either number moves, the two
     * stop agreeing here rather than in a frame nobody looks at. */
    JCE_EDITOR_VP_FULLSCREEN_BASE_OFFSET      = 20,
    JCE_EDITOR_VP_SSGI_OFFSET                 = 26,
    JCE_EDITOR_VP_SSGI_COMPOSITE_OFFSET       = 27,
    JCE_EDITOR_VP_FULLSCREEN_COMPOSITE_OFFSET = 28,
    /* Editor gizmo / grid / selection overlay. */
    JCE_EDITOR_VP_OVERLAY_OFFSET              = 29,
    /* PostFX chain: jce_postfx.c claims view_base .. view_base+20. */
    JCE_EDITOR_VP_POSTFX_BASE_OFFSET          = 30,
    JCE_EDITOR_VP_POSTFX_COUNT                = 21,
    JCE_EDITOR_VP_POSTFX_COMPOSITE_OFFSET     = 51,
    /* ── tail: everything that must run AFTER the scene colour pass ── */
    JCE_EDITOR_VP_UI_OFFSET                   = 58,
    JCE_EDITOR_VP_UPSCALE_OFFSET              = 59,
    JCE_EDITOR_VP_SCREENSHOT_OFFSET           = 60,
    JCE_EDITOR_VP_TSR_OFFSET                  = 61,   /* uses +61 and +62 */
    JCE_EDITOR_VP_TSR_COUNT                   = 2,
    /* One past the highest offset any editor viewport pass uses. */
    JCE_EDITOR_VP_OFFSET_END                  = 63
};

/* The Game View's base now lives in <jce/renderer/jce_views.h>, in the same
 * table as the Scene View's, because it is not an editor detail: it is a
 * SECOND scene-renderer base, and the engine's own band widths are what decide
 * how far apart the two have to be.  It was 80, which is 77 above the Scene
 * View -- forty ids short of the 117 a base actually claims once point-cube
 * shadows are on. */
constexpr uint16_t JCE_EDITOR_GAME_VIEW_BASE = JCE_VIEW_EDITOR_GAME;

/* Gate 1 -- no editor pass may sit on a scene-renderer-named id. */
#define JCE_EDITOR_VP_ASSERT_FREE(off)                                     \
    static_assert(!JCE_VIEW_SR_OFFSET_RESERVED(off),                       \
                  #off " collides with a bgfx view id the scene renderer "  \
                  "names: it would draw before the colour pass, then be "   \
                  "erased by that pass's clear")
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_SSR_COMPOSITE_OFFSET);
/* The planar reflection composite shares this id with SSR's, by submission
 * order -- jce_views.h says why that is what the two passes are rather than a
 * shortage of ids.  If the engine ever moves it, this stops compiling here
 * rather than in a frame where one reflection silently replaces the other. */
static_assert(JCE_EDITOR_VP_SSR_COMPOSITE_OFFSET
                  == JCE_VIEW_SR_REFLECTION_COMPOSITE_OFFSET,
              "editor and engine disagree about the reflection composite view");
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_FULLSCREEN_BASE_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_FULLSCREEN_COMPOSITE_OFFSET);
/* The fullscreen chain must stop before SSGI's march, and SSGI's two ids must
 * be the ones the engine names.  Both are compile-time facts; a mismatch here
 * is a pass silently overwriting another. */
static_assert(JCE_EDITOR_VP_FULLSCREEN_BASE_OFFSET + JCE_VIEW_SR_FULLSCREEN_MAX
                  <= JCE_EDITOR_VP_SSGI_OFFSET,
              "the fullscreen chain would run into SSGI's march view");
static_assert(JCE_EDITOR_VP_SSGI_OFFSET == JCE_VIEW_SR_SSGI_OFFSET,
              "editor and engine disagree about SSGI's march view");
static_assert(JCE_EDITOR_VP_SSGI_COMPOSITE_OFFSET
                  == JCE_VIEW_SR_SSGI_COMPOSITE_OFFSET,
              "editor and engine disagree about SSGI's composite view");
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_OVERLAY_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_POSTFX_BASE_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_POSTFX_BASE_OFFSET +
                          JCE_EDITOR_VP_POSTFX_COUNT - 1);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_POSTFX_COMPOSITE_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_UI_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_UPSCALE_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_SCREENSHOT_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_TSR_OFFSET);
JCE_EDITOR_VP_ASSERT_FREE(JCE_EDITOR_VP_TSR_OFFSET + JCE_EDITOR_VP_TSR_COUNT - 1);
#undef JCE_EDITOR_VP_ASSERT_FREE

/* Gate 2 -- each viewport's span must fit before the next fixed id and before
 * the other viewport's base. */
static_assert(JCE_VIEW_EDITOR_SCENE + JCE_EDITOR_VP_UI_OFFSET >
              JCE_VIEW_EDITOR_PREVIEW,
              "scene-view tail overlaps the fixed material-preview view id");
static_assert(JCE_VIEW_EDITOR_SCENE + JCE_EDITOR_VP_OFFSET_END <=
              JCE_VIEW_EDITOR_PICK,
              "scene-view span overruns the fixed pick view id");
static_assert(JCE_VIEW_EDITOR_SCENE + JCE_EDITOR_VP_OFFSET_END <=
              JCE_EDITOR_GAME_VIEW_BASE,
              "scene-view span overruns the game-view base");
static_assert(JCE_EDITOR_GAME_VIEW_BASE + JCE_EDITOR_VP_OFFSET_END <=
              JCE_VIEW_IMGUI,
              "game-view span overruns the ImGui view");

/* Gate 3 -- the spacing that a static_assert inside ONE viewport could never
 * check: a scene-renderer base claims up to JCE_VIEW_SR_POINT_CUBE_OFFSET +
 * COUNT - 1 (116, with point-cube shadows on), so the two bases must be at
 * least that far apart or the Scene View's cube band lands inside the Game
 * View's range.  This is R3, and it is why the Game View moved from 80 to 120. */
static_assert(JCE_EDITOR_GAME_VIEW_BASE - JCE_VIEW_EDITOR_SCENE >
              JCE_VIEW_SR_POINT_CUBE_OFFSET + JCE_VIEW_SR_POINT_CUBE_COUNT - 1,
              "the two editor viewport bases are closer together than one "
              "scene-renderer base actually claims: with point-cube shadows "
              "on, the Scene View's cube band would land inside the Game "
              "View's range");
static_assert(JCE_EDITOR_GAME_VIEW_BASE + JCE_VIEW_SR_POINT_CUBE_OFFSET +
              JCE_VIEW_SR_POINT_CUBE_COUNT - 1 < JCE_VIEW_IMGUI,
              "the Game View's point-cube band overruns the ImGui view");

/* Declare this viewport's OWN view ids to the engine's ownership guard, once
 * per frame per viewport.
 *
 * The guard (jce_view_bands_claim) was written for exactly the failure this
 * header documents above, and it never fired on it: only the scene renderer
 * and the postfx chain ever declared anything, so the editor's passes were
 * invisible to it.  The compile-time static_asserts are the primary gate for
 * the same-base case; this catches what a static_assert cannot -- a CROSS-base
 * overlap (one viewport's band reaching into another's) and any future engine
 * band that grows into this range at runtime.
 *
 * Three runs, not one: the postfx chain (base+30..+50) declares itself as
 * "postfx", so claiming it here would report a conflict with ourselves. */
inline void jce_editor_viewport_claim_view_band(const char *owner,
                                                uint16_t view_base)
{
    if (!jce_view_bands_enabled()) return;
    jce_view_bands_claim(owner,
        (uint16_t)(view_base + JCE_EDITOR_VP_SSR_COMPOSITE_OFFSET),
        (uint16_t)(JCE_EDITOR_VP_OVERLAY_OFFSET -
                   JCE_EDITOR_VP_SSR_COMPOSITE_OFFSET + 1));
    jce_view_bands_claim(owner,
        (uint16_t)(view_base + JCE_EDITOR_VP_POSTFX_COMPOSITE_OFFSET), 1u);
    jce_view_bands_claim(owner,
        (uint16_t)(view_base + JCE_EDITOR_VP_UI_OFFSET),
        (uint16_t)(JCE_EDITOR_VP_TSR_OFFSET + JCE_EDITOR_VP_TSR_COUNT -
                   JCE_EDITOR_VP_UI_OFFSET));
}

/* Composite the volumetric-fog (+16) and SSR (+19) render targets back over the
 * bridge's colour RT — after the scene draws into it, before overlays / PostFX.
 * Both are no-ops when the effect was not active this frame. */
inline void jce_editor_viewport_composite_fog_ssr(JceSceneRenderer   *sr,
                                                  JceOffscreenTarget *bridge,
                                                  uint16_t            view_base,
                                                  bool                fog_enabled)
{
    if (!sr || !bridge) return;
    const uint16_t dst_fb = jce_offscreen_target_get_frame_buffer(bridge);
    if (fog_enabled)
        jce_scene_renderer_composite_fog(sr, (uint16_t)(view_base + 16), dst_fb);
    jce_scene_renderer_composite_ssr(
        sr, (uint16_t)(view_base + JCE_EDITOR_VP_SSR_COMPOSITE_OFFSET), dst_fb);
    /* SAME VIEW, SECOND -- see jce_views.h.  The static_assert above ties this
     * offset to the engine's, so the runtime and the two viewports cannot
     * drift into compositing the two reflections in a different order. */
    JceFrameBufferHandle planar_dst = { dst_fb };
    jce_scene_renderer_composite_planar(
        sr, (uint16_t)(view_base + JCE_EDITOR_VP_SSR_COMPOSITE_OFFSET), planar_dst);
    /* SSGI composites at base+27, one above its own march at base+26 and below
     * the postfx base at base+30 that reads the colour it writes into.  Both
     * viewports and the shipped runtime use the SAME offset -- a pass wired
     * into one of them only is exactly the editor-vs-build divergence this
     * tree keeps finding, and the two static_asserts above the enum are that
     * agreement as a compile-time gate. */
    JceFrameBufferHandle ssgi_dst = { dst_fb };
    jce_scene_renderer_composite_ssgi(
        sr, (uint16_t)(view_base + JCE_EDITOR_VP_SSGI_COMPOSITE_OFFSET),
        ssgi_dst);
}


/* Run project-authored HDR full-screen stages and fold their output back into
 * the bridge.  This gives later overlays and the ordinary PostFX chain one
 * canonical color target in both editor viewports. */
inline bool jce_editor_viewport_apply_fullscreen_effects(
    JceSceneRenderer *sr, JceScene *scene, const JceCamera *camera,
    JceOffscreenTarget *bridge, uint16_t view_base, int viewport_id,
    uint32_t width, uint32_t height, float dt_sec)
{
    if (!sr || !scene || !camera || !bridge || width == 0 || height == 0)
        return false;
    JceTextureHandle color = {
        jce_offscreen_target_get_color_texture(bridge) };
    JceTextureHandle depth = {
        jce_offscreen_target_get_depth_texture(bridge) };
    if (!jce_gfx_texture_valid(color)) return false;

    JceTextureHandle output = jce_scene_renderer_apply_fullscreen_effects(
        sr, scene, camera, color, depth, width, height,
        (uint16_t)(view_base + JCE_EDITOR_VP_FULLSCREEN_BASE_OFFSET),
        viewport_id, JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX, dt_sec);
    if (!jce_gfx_texture_valid(output) || output.idx == color.idx)
        return false;

    jce_offscreen_target_composite_texture(bridge,
        (uint16_t)(view_base +
                   JCE_EDITOR_VP_FULLSCREEN_COMPOSITE_OFFSET),
        output.idx, (uint16_t)width, (uint16_t)height,
        jce_renderer_origin_bottom_left());
    return true;
}

/* Fold the tone-mapped PostFX output back into the bridge so the canvas UI
 * lands AFTER post-fx — the shipped runtime's scene→postfx→UI order.  The
 * bridge then holds the final LDR frame, so both display and read-back capture
 * read the bridge.  The composite view (+21) sits past the postfx chain's worst
 * case (+18).  flip_v: on bottom-left-origin backends (GL) the postfx RT's
 * sampling orientation is inverted relative to the bridge/canvas convention.
 * Returns true when a composite happened — the caller then drops its postfx
 * output handle, because the bridge IS the frame. */
inline bool jce_editor_viewport_composite_postfx(JceOffscreenTarget *bridge,
                                                 uint16_t            view_base,
                                                 uint16_t            postfx_tex,
                                                 uint32_t            width,
                                                 uint32_t            height)
{
    if (!bridge || postfx_tex == UINT16_MAX) return false;
    const uint16_t comp_view = (uint16_t)(
        view_base + JCE_EDITOR_VP_POSTFX_COMPOSITE_OFFSET);
    jce_offscreen_target_composite_texture(bridge, comp_view, postfx_tex,
                                           (uint16_t)width, (uint16_t)height,
                                           jce_renderer_origin_bottom_left());
    return true;
}

/* View id for the ECS-UI (Canvas) overlay pass.  With PostFX active it must
 * draw after the +21 composite (crisp, un-tonemapped UI, matching the runtime);
 * without PostFX it draws right after the scene colour and the fog composite. */
inline uint16_t jce_editor_viewport_ui_overlay_view(uint16_t view_base,
                                                    bool     postfx_composited)
{
    (void)postfx_composited;
    return (uint16_t)(view_base + JCE_EDITOR_VP_UI_OFFSET);
}

/* Submit a headless read-back capture of the texture the viewport DISPLAYS.
 * Once PostFX has been composited the bridge holds the final frame; otherwise
 * the postfx output is the source and reads back bottom-up on every backend.
 * The blit view (+24) is past the postfx chain (+18), the postfx→bridge
 * composite (+21) and the canvas-UI overlay (+22), so the read-back sees the
 * fully composited frame of the SAME bgfx frame.  Poll
 * jce_renderer_readback_capture_poll() each frame until it returns non-zero. */
inline bool jce_editor_viewport_screenshot_submit(JceOffscreenTarget *bridge,
                                                  uint16_t            view_base,
                                                  uint16_t            postfx_tex,
                                                  uint16_t            width,
                                                  uint16_t            height,
                                                  const char         *path)
{
    if (!bridge) return false;
    uint16_t source = postfx_tex;
    int      yflip  = 1;   /* postfx RT reads back bottom-up on every backend */
    if (source == UINT16_MAX) {
        source = jce_offscreen_target_get_color_texture(bridge);
        yflip  = jce_renderer_origin_bottom_left() ? 1 : 0;
    }
    if (source == UINT16_MAX) return false;
    const uint16_t blit_view =
        (uint16_t)(view_base + JCE_EDITOR_VP_SCREENSHOT_OFFSET);
    return jce_renderer_readback_capture_submit(source, blit_view,
                                                width, height, path, yflip);
}

/* ── Occlusion culling ────────────────────────────────────────────── */

/* Per-entity hardware-query occlusion culling is OPT-IN (JCE_ENABLE_OCCLUSION=1).
 *
 * It was on by default and measurement says it cannot pay for itself here.  The
 * query budget is maxOcclusionQueries / share_count = 256 / 2 = 128 for an
 * editor viewport, and JCE_DBG_OCC_STATS reports what that buys:
 *
 *   street_demo + 200k   tracked 27176, tested 105 (0.39%), occluded 101,
 *                        visible 0 -- it decides nothing about 99.6% of the
 *                        scene, and a screenshot A/B says the 0.4% it does
 *                        decide is WRONG (0.216% of pixels differ against a
 *                        0.059% run-to-run floor: it is culling geometry that
 *                        is visible)
 *   graveyard            tracked 178, tested 133 (74.7%), occluded 0 -- full
 *                        coverage, nothing culled, pure cost
 *
 * The cost is not small: one proxy draw per tracked entity, each of which
 * forces bgfx's D3D12 backend to flush its ExecuteIndirect batch, plus a
 * per-frame scan of the whole 65536-slot result table to poll at most 128
 * answers.  On the 200k bench it is 11-12% of the submit loop and triples the
 * draw count by breaking instance runs (147 draws with it on, 45 with it off).
 *
 * Turning it OFF draws strictly MORE -- entity_visible returns true whenever
 * there is no query result -- so this default cannot hide geometry.
 *
 * Shipping engines did not abandon occlusion culling; they abandoned ONE DRAW
 * PER QUERY. Unreal batches queries and prefers Hi-Z, Godot 4 rasterises
 * software occluders (Embree), Unity 6 uses a compute Hi-Z pass, id Tech and
 * Frostbite use a software depth pyramid. JCE already has Hi-Z occlusion
 * (JCE_HIZ_OCCLUSION, jce_gpu_scene.c) -- that is the path worth investing in,
 * and it is unaffected by this switch. */
/* Resolution order, matching every other perf feature: env first (forensic
 * override), then the .rp.json / preset tri-state, then the built-in default
 * of OFF explained above.
 *
 * The env var is JCE_ENABLE_OCCLUSION, an opt-IN.  Four comments and both
 * call sites used to name JCE_DISABLE_OCCLUSION, an opt-OUT that NO getenv in
 * this tree has ever read -- so on a default run the editor logged
 * "JCE_DISABLE_OCCLUSION set — occlusion culling OFF" on a machine where
 * nothing of the sort was set, and a reader could only conclude a diagnostic
 * had been left on.  It had not; the feature is off by design.  The messages
 * say that now. */
inline bool jce_editor_viewport_occlusion_enabled(void)
{
    const char *en = getenv("JCE_ENABLE_OCCLUSION");
    if (en && en[0])
        return en[0] != '0';

    JceEditorConfig cfg;
    if (jce_editor_config_load(&cfg))
        return cfg.viewport_occlusion;
    return false;   /* built-in: off, for the reason above */
}

/* True when the env var is what decided it, so the caller can say so instead
 * of implying a setting. */
inline bool jce_editor_viewport_occlusion_forced_by_env(void)
{
    const char *en = getenv("JCE_ENABLE_OCCLUSION");
    return en && en[0];
}

/* Create a viewport's own two-pass GPU-query occlusion culler.  Each viewport
 * needs its OWN instance — per-entity visibility history must not be shared
 * between two cameras — on its OWN depth-only proxy view, so the two never
 * clobber each other when both panels render in the same bgfx frame.
 * proxy_view_id < 0 keeps the config default.  Only the 'color' program is
 * needed for the AABB proxy draw.  Returns NULL when hardware queries are
 * unsupported; the caller logs that in its own voice and degrades silently to
 * always-visible. */
inline JceOcclusionCuller *jce_editor_viewport_create_occlusion_culler(
    JceRenderer *renderer, int proxy_view_id)
{
    if (!renderer) return nullptr;

    JceShaderSet oc_shaders;
    memset(&oc_shaders, 0, sizeof(oc_shaders));
    JceShaderHandle ch = jce_renderer_get_program_color(renderer);
    oc_shaders.color.idx = ch.idx;

    JceOcclusionConfig oc_cfg = jce_occlusion_config_default();
    oc_cfg.query_pool_share_count = 2;
    if (proxy_view_id >= 0)
        oc_cfg.view_id = (uint16_t)proxy_view_id;

    return jce_occlusion_culler_create(&oc_cfg, &oc_shaders);
}

#endif /* JCE_EDITOR_VIEWPORT_COMMON_H */
