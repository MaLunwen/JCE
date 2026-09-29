/*
 * jce_views.h  bgfx view ID assignments.
 *
 * bgfx renders views in ID order. Lower IDs render first.
 * This header defines the canonical view layout for JCE.
 */

#ifndef JCE_VIEWS_H
#define JCE_VIEWS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

/* -- View IDs ------------------------------------------------------- */

/* Main 3D scene (perspective camera, depth test, lit meshes). */
#define JCE_VIEW_MAIN_3D    0

/* Blendshape (morph target) deform: ONE compute dispatch per morph-bearing
 * primitive, writing the vertex buffer the colour pass then DRAWS.
 *
 * BELOW EVERY SCENE BASE, and that is the entire reason for the number.  bgfx
 * executes views in ascending id order, so a dispatch on any id above a scene
 * base would run AFTER the draw that reads its output and the mesh would be
 * one frame behind its own animation -- visible on a face rig, where the
 * weights change every frame by definition.  1 is free: 0 and 2 are the
 * renderer's direct-to-backbuffer pair above and below it, and the lowest
 * scene base is the editor Scene View at 3.
 *
 * NOT a band: one id serves every primitive and every instance, because a
 * compute dispatch carries its own buffer bindings and nothing about a view's
 * state (rect, framebuffer, transform) is read for one. */
#define JCE_VIEW_MORPH_DEFORM 1

/* Debug overlay (bgfx debug text, profiler). */
#define JCE_VIEW_DEBUG       2

/* Editor scene viewport render target (off-screen scene panel). */
#define JCE_VIEW_EDITOR_SCENE 3

/* RETIRED: JCE_VIEW_SHADOW_BASE / _0.._4 (10..14), the one-view-per-light
 * shadow passes from before CSM.  ZERO consumers in the tree -- and they named
 * ids that the editor Scene View's base of 3 actively writes as its own core
 * offsets 7..11.  A dead name on a live id is how a future pass gets told
 * "10 is the shadow view, it's fine".  The ids stay recorded in
 * contracts/view-reservations.txt; only the names are gone. */

/* Reserved range for post-processing (Phase 4). */
#define JCE_VIEW_POST_BASE   20

/* Standalone runtime (drop-in main) offscreen scene target, used when the
 * authored scene postfx chain is active.  Its own sub-passes are base-relative
 * (shadow base+10..+13, fog base+16 -> 40..46); the postfx pipeline is re-based
 * to 100 (jce_postfx_set_view_base) so the two ranges never collide. */
#define JCE_VIEW_RUNTIME_GAME 30

/* Editor gizmo / grid / selection overlay — drawn AFTER PostFX so it
 * doesn't get tone-mapped or bloomed. Targets the postfx output FBO. */
#define JCE_VIEW_EDITOR_OVERLAY 50

/* Editor material-graph live preview (offscreen sphere). Sits between
 * the editor's main scene viewport (3) and the post-fx / overlay
 * range, so the preview can be drawn while editor scene rendering is
 * still in progress without view-ordering surprises. */
#define JCE_VIEW_EDITOR_PREVIEW 60

/* Hidden editor object-ID picking pass.  Separate FBO/readback target,
 * after regular scene + preview views and before UI. */
#define JCE_VIEW_EDITOR_PICK 70
/* One-pixel GPU blit into a readback staging texture. */
#define JCE_VIEW_EDITOR_PICK_READBACK 71

/* Editor Game View render target.  A SECOND scene-renderer base, and therefore
 * the reason the spacing below is not a matter of taste.
 *
 * A scene-renderer base NAMES up to offset 116 (the point-shadow cube band),
 * and the editor's own passes on top of it reach 62, so ONE viewport's claim is
 * 117 ids wide.  It was 80, which is 77 above the Scene View at 3 -- forty ids
 * short.  With point-cube shadows on (r.point_shadows, default off) the Scene
 * View's cube band 103..119 therefore sat inside the Game View's range, a
 * CROSS-BASE overlap no static_assert inside one viewport can see.
 *
 * 120 makes the two spans exactly adjacent: 3..119 and 120..236.  The room came
 * from retiring JCE_VIEW_INIT_CLEAR below, which nothing had used for years. */
#define JCE_VIEW_EDITOR_GAME 120

/* ── Scene-renderer base-relative reservations ─────────────────────────
 *
 * jce_scene_renderer_render(base, ...) does not only draw into base+0.  It
 * NAMES a set of base-relative ids for its producer passes and hands them to
 * bgfx_set_view_order() so they sort BEFORE the colour pass.  bgfx's remap
 * window is contiguous, so the builder also appends every id in
 * [base, highest] it did not name -- but that FILLER all sorts AFTER every
 * named id, which is why a foreign pass parked on a filler id still draws
 * after the scene.
 *
 * A foreign pass on a NAMED id is the failure this block exists to prevent: it
 * inherits that id's early sort position, rasterises into the target, and is
 * then erased by the colour pass's clear.  Nothing errors and nothing logs --
 * the pass simply produces no pixels.  That is what happened to the editor's
 * ECS-UI Canvas overlay, which sat exactly on JCE_VIEW_SR_DYN_CSM_OFFSET: the
 * Scene View and the Game View both stopped drawing authored UI while the
 * shipped runtime (JCE_VIEW_UI, outside every window) kept drawing it.
 *
 * Any subsystem that borrows a scene-renderer base -- the editor viewports are
 * the only ones today -- MUST keep its own ids out of the named set.
 * JCE_VIEW_SR_OFFSET_RESERVED() answers that in one expression so the check can
 * be a static_assert at the declaration site rather than a comment. */

/* base+0 .. base+JCE_VIEW_SR_CORE_SPAN-1 -- the contiguous run the builder
 * pushes by name.  Spelled out slot by slot because the one slot that was NOT
 * written down here is the one that acquired two owners:
 *
 *   +0   scene colour
 *   +1   velocity prepass                    (jce_sr_cull.c)
 *   +2   SSAO sample  \ jce_ssao_render() takes ONE view id and uses it AND
 *   +3   SSAO blur    / the next one (jce_ssao.c: v_sample, v_blur = +0, +1)
 *   +4..+8   local shadow atlas: 1 clear + 4 tiles
 *   +9   GPU particle simulate/emit, and the GPU-cull compact
 *   +10  simple shadow map
 *   +11..+14 CSM cascade draws
 *   +15  volumetric fog render
 *   +16  fog composite
 *   +17  underwater absorption
 *
 * +18 (SSR ray-march) sits deliberately OUTSIDE this run: SSR reads the lit
 * colour of the frame it runs in, so it must sort AFTER base+0, and being
 * unnamed is exactly what puts it in the builder's filler tail.
 *
 * base+3 used to be declared "free" in jce_sr_internal.h and carry the
 * GPU-cull counter reset.  It is not free: it is SSAO's blur, because
 * jce_ssao_render consumes two views from one argument.  With SSAO and
 * r.gpu_driven both on, the builder pushed base+3 to sort FIRST and SSAO's
 * blur therefore ran before SSAO's own sample pass. */
#define JCE_VIEW_SR_CORE_SPAN          18u

/* SSAO's pair, named so nothing reclaims the second one again. */
#define JCE_VIEW_SR_SSAO_OFFSET         2u
#define JCE_VIEW_SR_SSAO_COUNT          2u

/* GPU-driven cull counter RESET (opt-in: r.gpu_driven).  It needs to be a
 * SEPARATE view that sorts before the compact at base+9, so bgfx inserts a
 * cross-view compute barrier between the reset's zero and the compact's
 * atomics -- same-view dispatches keep the counter in UAV state and get none.
 *
 * 63, and the history is the point.  Every slot in the contiguous run above is
 * taken and 19..62 belongs to consumers of a scene-renderer base (the editor's
 * SSR composite, fullscreen stages, overlay, PostFX chain, composite and tail)
 * or to the dynamic-CSM band, so this first went to 57 -- the one gap below the
 * editor's tail.  57 is absolute id 60 at the Scene View's base of 3, and
 * JCE_VIEW_EDITOR_PREVIEW is 60.  The commit that removed one two-owner view id
 * created another, against an ABSOLUTE id, which is the one axis no per-base
 * static_assert can reach.
 *
 * 63 is free at every live base (3 -> 66, 30 -> 93, 120 -> 183), clear of every
 * fixed id and past the editor's consumer band.  57 is not released: see the
 * frozen set below. */
#define JCE_VIEW_SR_GPU_CULL_RESET_OFFSET 63u

/* CAMERA STACKING -- one view per OVERLAY camera, drawn into the base camera's
 * colour target after the base camera's own pass.
 *
 * 64..66, immediately above the GPU-cull reset, and the reasoning is that
 * constant's verbatim: 19..62 belongs to consumers of a scene-renderer base
 * (the editor's SSR composite, fullscreen stages, overlay, PostFX chain,
 * composite and tail), so the first free offsets past the editor's band are 64
 * upward.  Free at every live base -- 3 -> 67..69, 30 -> 94..96, 120 ->
 * 184..186 -- and clear of every absolute id (EDITOR_PREVIEW 60, EDITOR_PICK
 * 70/71).
 *
 * ORDER, not id, is what makes this correct, and it is why these are NAMED in
 * jce_scene_renderer_view_order.c rather than left to the filler tail.  An
 * overlay writes into the scene colour, so it has to run AFTER base+0 and
 * BEFORE everything that reads that colour: SSR at base+18, the project
 * fullscreen chain at base+20, SSGI's composite at base+27 and the PostFX
 * chain at base+30.  Ascending id order would have put 64 after all of them --
 * an overlay drawn on top of an already tone-mapped image, which is the one
 * thing Unity's camera stack is defined not to do.  The builder pushes these
 * immediately after the colour view instead.
 *
 * THREE, because JceCameraComponent.stack_index documents 1..3 as the overlay
 * range and 0 as the base.  A fourth overlay is refused and said so, not
 * silently dropped. */
#define JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET 64u
#define JCE_VIEW_SR_CAMERA_OVERLAY_MAX     3u

/* SSGI: one view marches the bounce, the next adds it onto the scene colour.
 *
 * 26/27 and not something in the 60s, because BOTH have to be BELOW the
 * editor's postfx base (base+30): the composite writes into the scene colour
 * the postfx chain then reads, and bgfx runs views in ascending id order, so a
 * higher id would add the bounce to an already-tonemapped image.  Everything
 * from 18 to 29 was claimed -- 18/19 SSR, 20..27 the project fullscreen chain,
 * 28 its composite, 29 the overlay -- and 0..17 is this renderer's own.  These
 * two are taken off the fullscreen chain, which is now capped at
 * JCE_VIEW_SR_FULLSCREEN_MAX stages; see that constant for why the cap is a
 * repair and not only a cost.
 *
 * The composite MUST sort after the march or it adds last frame's RT. */
#define JCE_VIEW_SR_SSGI_OFFSET           26u
#define JCE_VIEW_SR_SSGI_COMPOSITE_OFFSET 27u

/* THE SCREEN-SPACE REFLECTION COMPOSITE, shared by SSR and the planar probe.
 *
 * Both blend a premultiplied reflection over the scene colour with the same
 * "over" state, which makes them one STAGE with two producers rather than two
 * stages -- so they share one view id and are ordered by submission, the
 * planar composite second because a planar reflection is the more accurate
 * answer wherever it applies and should win over the screen-space guess.
 *
 * IT IS NOT A SPARE-ID DECISION, though the band has none: 0..17 are frozen,
 * 18 is the SSR march, 20..25 the project fullscreen chain, 26..27 SSGI,
 * 28 its fold-back, 29 the editor overlay and 30.. postfx.  Giving the planar
 * composite its own id would have meant shrinking a documented cap.  Sharing
 * is what the two passes actually are. */
#define JCE_VIEW_SR_REFLECTION_COMPOSITE_OFFSET 19u

/* Project-authored HDR fullscreen stages, base+20 upward, ONE VIEW EACH.
 *
 * THE CAP IS THE POINT.  jce_sr_fullscreen_effect.c assigned
 * `view_id_base + i` with no upper bound, so a project with enough stages
 * silently walked over the fold-back composite (base+28), the editor overlay
 * (base+29) and into the postfx chain (base+30) -- bgfx view state is
 * last-write-wins, so the symptom is a pass that stops producing pixels, not
 * an error.  Six is what fits under SSGI's 26 with the chain starting at 20;
 * the number is a consequence of the layout, not a taste. */
#define JCE_VIEW_SR_FULLSCREEN_MAX         6u

/* Dual shadow-map DYNAMIC cascade atlas: 1 full-atlas depth clear + 4 tiles. */
#define JCE_VIEW_SR_DYN_CSM_OFFSET     52u
#define JCE_VIEW_SR_DYN_CSM_COUNT       5u

/* Omnidirectional point-shadow cube tiles (opt-in: r.point_shadows /
 * JCE_POINT_CUBE_SHADOWS).  Reserved unconditionally here: a reservation that
 * moves with a cvar is not a reservation. */
#define JCE_VIEW_SR_POINT_CUBE_OFFSET 100u
#define JCE_VIEW_SR_POINT_CUBE_COUNT   17u

/* True when `off` is a base-relative id the scene renderer NAMES. */
/* ── FROZEN reservation set — APPEND ONLY, NEVER NARROW ────────────────
 *
 * This is NOT the same thing as the constants above, and the difference is the
 * whole point.
 *
 *   The LIVE constants say where a pass IS.  They drive the order builder, and
 *   they move when a band moves.
 *
 *   This FROZEN set says where a FOREIGN pass may NEVER be.  It is the union of
 *   every base-relative range the scene renderer has EVER named, and a band
 *   that migrates is not removed from it.  Monotone non-decreasing by
 *   construction.
 *
 * The editor's static_asserts test THIS set, not the live one.  Two reasons,
 * both load-bearing:
 *
 *   1. A predicate assembled from the live constants cannot disagree with the
 *      builder that uses them.  Measured: a control that changed
 *      JCE_VIEW_SR_DYN_CSM_COUNT changed the builder AND the predicate together
 *      and every gate stayed green.  Such a guard can only catch a band added
 *      as a fresh literal -- never one that moved.
 *
 *   2. The day a band migrates, a live-derived predicate stops reserving the
 *      ids it vacated, and an editor pass may legally take one -- silently,
 *      with every gate green.  That is the ORIGINAL defect (an editor pass on a
 *      renderer-named id, drawing and then being erased) re-created one level
 *      up, by the very mechanism meant to prevent it.
 *
 * ADDING a band: append a new range, never edit an existing one.  RETIRING a
 * band: delete the LIVE constant, leave the frozen range.  Reclaiming a frozen
 * range for a foreign pass is a deliberate act that must be argued for in the
 * commit that narrows this set, and contracts/view-reservations.txt is what
 * makes that argument visible outside the compiler.
 *
 * Ranges are [LO, HI] INCLUSIVE. */
#define JCE_VIEW_SR_FROZEN_RANGE_COUNT 6u

/* [0]  the contiguous core run: colour, velocity, SSAO's pair, local-shadow
 *      atlas, GPU particle/cull compact, shadow + CSM cascades, volumetric fog,
 *      fog composite, underwater. */
#define JCE_VIEW_SR_FROZEN_0_LO    0u
#define JCE_VIEW_SR_FROZEN_0_HI   17u

/* [1]  dual-shadow dynamic CSM atlas (52..56), and 57 where the GPU-cull
 *      counter reset briefly lived.  57 IS VACATED AND STILL FROZEN -- the
 *      reset has moved on to 63 (range [3]) because 57 was absolute id 60 at
 *      base 3, i.e. JCE_VIEW_EDITOR_PREVIEW.  Keeping 57 is the append-only
 *      rule doing its job the first time a band actually moved: a foreign pass
 *      must not be handed an id the renderer has ever named. */
#define JCE_VIEW_SR_FROZEN_1_LO   52u
#define JCE_VIEW_SR_FROZEN_1_HI   57u

/* [2]  omnidirectional point-shadow cube tiles (opt-in at runtime; reserved
 *      unconditionally here -- a reservation that moves with a cvar is not a
 *      reservation). */
#define JCE_VIEW_SR_FROZEN_2_LO  100u
#define JCE_VIEW_SR_FROZEN_2_HI  116u

/* [3]  GPU-cull counter reset, current home.  Appended rather than folded
 *      into [1]: 58..62 between them is the editor's own tail band, and
 *      widening [1] to cover 63 would freeze the editor's ids out from under
 *      it. */
#define JCE_VIEW_SR_FROZEN_3_LO   63u
#define JCE_VIEW_SR_FROZEN_3_HI   63u

/* SSGI: the bounce march and its composite.  APPENDED, never edited -- the
 * frozen set is a union that may only grow, so a foreign pass may not use
 * 26 or 27 from a scene-renderer base from here on. */
#define JCE_VIEW_SR_FROZEN_4_LO   26u
#define JCE_VIEW_SR_FROZEN_4_HI   27u

/* [5]  camera-stack overlay views.  APPENDED, never edited: a foreign pass may
 *      not use 64..66 from a scene-renderer base from here on. */
#define JCE_VIEW_SR_FROZEN_5_LO   64u
#define JCE_VIEW_SR_FROZEN_5_HI   66u

/* True when `off` is a base-relative id a foreign pass must not take. */
/* EVERY range, and the count above says how many there are.  This tested 0..3
 * and stopped, so range [4] -- SSGI's 26..27 -- was frozen everywhere except in
 * the one expression the editor's static_asserts actually call: the set said
 * "never take base+26", the predicate said "go ahead".  Adding a range means
 * adding a clause here; there is no loop to write it in, so the clauses and
 * JCE_VIEW_SR_FROZEN_RANGE_COUNT are checked against each other by
 * tools/audit/check_view_reservations.py. */
#define JCE_VIEW_SR_OFFSET_RESERVED(off)                                 \
    (((unsigned)(off) >= JCE_VIEW_SR_FROZEN_0_LO &&                      \
      (unsigned)(off) <= JCE_VIEW_SR_FROZEN_0_HI) ||                     \
     ((unsigned)(off) >= JCE_VIEW_SR_FROZEN_1_LO &&                      \
      (unsigned)(off) <= JCE_VIEW_SR_FROZEN_1_HI) ||                     \
     ((unsigned)(off) >= JCE_VIEW_SR_FROZEN_2_LO &&                      \
      (unsigned)(off) <= JCE_VIEW_SR_FROZEN_2_HI) ||                     \
     ((unsigned)(off) >= JCE_VIEW_SR_FROZEN_3_LO &&                      \
      (unsigned)(off) <= JCE_VIEW_SR_FROZEN_3_HI) ||                     \
     ((unsigned)(off) >= JCE_VIEW_SR_FROZEN_4_LO &&                      \
      (unsigned)(off) <= JCE_VIEW_SR_FROZEN_4_HI) ||                     \
     ((unsigned)(off) >= JCE_VIEW_SR_FROZEN_5_LO &&                      \
      (unsigned)(off) <= JCE_VIEW_SR_FROZEN_5_HI))

/* ImGui editor overlay (renders before UI overlay so HUD sits on top). */
#define JCE_VIEW_IMGUI       250

/* GPU occlusion-query proxy pass (runtime).  MUST be a view nothing else
 * writes: bgfx view state is last-write-wins, and when this shared
 * JCE_VIEW_UI (254) the UI canvas's identity + pixel-ortho transform
 * clobbered the culler's camera transform — every world-space proxy box
 * outside the pixel ortho volume clipped to 0 samples and its entity was
 * PERMANENTLY false-culled (bridge/campfire in the elemental_serenity
 * repro).  Runs after the scene color views so queries test against the
 * final depth.  (253 stays the editor game-view culler's explicit id.) */
#define JCE_VIEW_OCCLUSION   252

/* The editor Game View's own occlusion proxy.  It has to be distinct from the
 * Scene View's (252) so the two viewports can carry independent query state in
 * one frame.  It was a bare 253 at its only call site with no macro and no
 * contract row -- which is how it came to sit one below a view whose own
 * comment says it "MUST be a view nothing else writes". */
#define JCE_VIEW_EDITOR_GAME_OCCLUSION 253

/* Impostor bake.  One view per octahedral cell, single frame, so the band is
 * sized by the WORST CASE grid and not by the grid actually requested:
 *   BASE-1            full-atlas clear
 *   BASE .. BASE+g^2-1  the g^2 cell views for the requested grid g
 *   BASE + MAX_GRID^2   the readback blit -- a FIXED id, so it does not move
 *                       down when g < MAX_GRID
 * These lived as private #defines in jce_impostor.c, which is why the id table
 * did not know about the band its own contract row recorded. */
#define JCE_VIEW_IMPOSTOR_BAKE_BASE 128
#define JCE_IMPOSTOR_BAKE_MAX_GRID  10
#define JCE_VIEW_IMPOSTOR_BAKE_BLIT     (JCE_VIEW_IMPOSTOR_BAKE_BASE + JCE_IMPOSTOR_BAKE_MAX_GRID * JCE_IMPOSTOR_BAKE_MAX_GRID)

/* Reflection probe capture: the blit that copies one finished cube face into
 * the read-back staging texture.  It must sort AFTER a full scene render, and
 * the highest viewport base (game view, 120) claims through 236, so this is
 * the first id past every base-relative claim.  The six FACE renders do not
 * need ids of their own -- they reuse the Scene View's base while both editor
 * viewports yield, the same modal arrangement as the impostor bake.
 *
 * Named here rather than left as a +N at the call site for the reason the
 * recorder's note below gives: an id derived by arithmetic in one .c is an id
 * the contract cannot see. */
#define JCE_VIEW_PROBE_CAPTURE_BLIT 237

/* Planar reflection: a SECOND, reduced scene render, from its own base.
 *
 * ABSOLUTE and above every scene base, which is what makes it one frame late
 * -- and it has to be.  bgfx runs views in ascending id order and the water
 * that samples the reflection draws in the scene's COLOUR view (base+0), so
 * no base-relative id can render before its consumer.  Reclaimed from the
 * 238-247 band this file records as RETIRED with no consumer; the argument
 * for reclaiming it is exactly this paragraph, which is what the header
 * above asks for.
 *
 * The reduced pass claims base+0 and, when a skybox is drawn, nothing more --
 * shadows, the depth pre-pass and every screen-space effect are off (see
 * JceSceneRenderConfig::reduced_pass).  Ten ids is generous. */
#define JCE_VIEW_PLANAR_REFLECTION_BASE 238
#define JCE_VIEW_PLANAR_REFLECTION_SPAN  10

/* The F9 screen recorder derives TWO views from JCE_VIEW_IMGUI: +1 to resolve
 * the ImGui FBO and +2 to blit it into the encoder's staging texture.  +2 is
 * 252, i.e. JCE_VIEW_OCCLUSION.  They do not collide in practice -- the
 * recorder runs after the frame's occlusion queries and the proxy writes no
 * colour -- but "in practice" is exactly the argument that was made for
 * base+52, so the ids are named here and recorded in the contract rather than
 * left to be re-derived from an addition at a call site. */
#define JCE_VIEW_RECORDER_RESOLVE (JCE_VIEW_IMGUI + 1)
#define JCE_VIEW_RECORDER_BLIT    (JCE_VIEW_IMGUI + 2)

/* 2D UI / RmlUi overlay (orthographic, no depth test — sprites, text,
 * debug HUD). Placed AFTER ImGui so the engine HUD is visible on top
 * of editor panels. */
#define JCE_VIEW_UI          254

/* RETIRED.  232..247 were reserved for clearing freshly-created render-target
 * textures, then that approach was abandoned -- view-id ordering cannot do it,
 * because bgfx runs every view each frame and the renderer's own writes to the
 * FBO go through lower ids that run BEFORE the clear.  Clears have been direct
 * CPU-zero uploads at texture creation ever since (jce_clear_freshly_created_
 * fbo), and a grep of the whole tree finds no consumer of these constants.
 *
 * The band is now RECLAIMED rather than "kept reserved": sixteen ids held for a
 * mechanism that does not exist is what left the two editor viewports unable to
 * be spaced far enough apart to be disjoint (see JCE_VIEW_EDITOR_GAME).  The
 * constants are deliberately not kept as deprecated aliases -- a stale
 * reservation that still compiles is how this one survived. */

/* ── View-id ownership guard ───────────────────────────────────────────
 *
 * bgfx view state is last-write-wins and view ORDER is a global remap, so two
 * subsystems that reach for the same id do not conflict loudly — one of them
 * silently stops producing pixels.  This guard makes each owner DECLARE what
 * it is about to bind, so an overlap is reported by name at the moment it
 * happens.
 *
 * It is declared HERE, in the public view-id header, rather than in an
 * engine-private one, because the subsystems that collide are not all inside
 * the engine: the editor's two viewports derive their passes from a scene-
 * renderer base and had no way to declare them at all.  That is why the guard
 * saw nothing while the ECS-UI overlay sat on top of the dynamic-CSM atlas —
 * only the scene renderer and the postfx chain were ever declaring, so the
 * one guard written for exactly this bug could not fire on it.
 *
 * Claims are per FRAME (a frame legitimately renders the scene several times —
 * scene viewport, game view, thumbnails — each from its own base), and
 * re-claiming a range you already hold this frame is not an error. */

JCE_EXTERN_C_BEGIN

/* Drop all claims.  Called once per frame by the engine, before any view
 * assignment; consumers do not call this. */
JCE_API void     jce_view_bands_begin_frame(void);

/* Declare that `owner` owns bgfx views [first, first + count).
 * `owner` must be a long-lived pointer (a string literal).
 *
 * Returns true when the range was free (or already held by the same owner).
 * On overlap: logs an error naming BOTH owners and the offending view ids,
 * once per distinct pair per frame, and returns false.  The caller is not
 * expected to act on the result — proceeding reproduces the old
 * last-write-wins behaviour, which is strictly better than refusing to render
 * — but a test can assert on it. */
JCE_API bool     jce_view_bands_claim(const char *owner,
                                      uint16_t first, uint16_t count);

/* Number of overlaps detected since the last begin_frame.  0 is the healthy
 * value. */
JCE_API uint32_t jce_view_bands_conflict_count(void);

/* Off by default in dist builds, on otherwise; JCE_VIEW_BAND_CHECK=0/1
 * overrides.  When off, claim() is a no-op that returns true. */
JCE_API bool     jce_view_bands_enabled(void);

JCE_EXTERN_C_END

/* NOTE: the render-target scrub helpers jce_clear_freshly_created_fbo() and
 * jce_zero_init_mem() used to be declared here.  jce_zero_init_mem returns a
 * bgfx type (bgfx_memory_t) and both are renderer-internal, so they were
 * moved to engine/src/renderer/jce_renderer_internal.h to keep the public
 * ABI free of bgfx types (audit R-D41 / rend-01-bgfx-type-in-public-abi). */

#endif /* JCE_VIEWS_H */
