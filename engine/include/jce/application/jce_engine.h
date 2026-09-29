/*
 * jce_engine.h  Engine bootstrap and lifecycle.
 *
 * Owns all subsystems (window, renderer, audio, input, PAK, app).
 * The application entry point delegates every callback to this module.
 *
 * This header is SDL-free; game/application code does not need SDL.
 */

#ifndef JCE_ENGINE_H
#define JCE_ENGINE_H


#include <jce/os/core/jce_defs.h>

#include <stddef.h>   /* size_t (jce_engine_set_app_desc_sized) */
#include <stdbool.h> /* bool (jce_engine_quit_requested, _forget_resolved_backend) */

JCE_EXTERN_C_BEGIN

/* Application result codes (match SDL_AppResult values). */
typedef enum JceAppResult {
    JCE_APP_CONTINUE = 0,   /* Keep running */
    JCE_APP_SUCCESS  = 1,   /* Quit successfully */
    JCE_APP_FAILURE  = 2    /* Quit with error */
} JceAppResult;

typedef struct JceEngine       JceEngine;
typedef struct JceAppDesc      JceAppDesc;
typedef struct JceInputActions JceInputActions;
struct JceConfig;
struct JceRenderer;

/* Register the application descriptor (IApp callbacks).
   Must be called before jce_engine_create().
   If not called, engine falls back to direct jce_app_* calls.

   ── Why there are two of these ──────────────────────────────────────
   The engine COPIES the descriptor by value.  A game built against an
   older SDK allocates an older, SMALLER JceAppDesc — the struct has grown
   more than once (window_width/window_height, then headless) — so copying
   sizeof(JceAppDesc) as the ENGINE currently sees it reads past the end of
   the caller's object, and the trailing fields come back as garbage.  It
   is an out-of-bounds read of somebody else's stack, and "headless" being
   randomly true is a spectacular way to find out.

   jce_engine_set_app_desc_sized() takes the CALLER's sizeof and copies
   min(caller, engine), zero-filling the rest, so a short descriptor is
   safe and a longer one (game newer than engine) has its unknown tail
   ignored.  Pass sizeof(JceAppDesc) as seen at YOUR compile time.

   The macro below routes consumer code through the sized form
   automatically, so simply rebuilding picks up the fix with no source
   change.  Engine-internal TUs skip it (they are always in lockstep, and
   jce_engine.c has to be able to DEFINE the plain symbol).  Define
   JCE_NO_APP_DESC_SIZE_SHIM if you need the real function symbol — e.g.
   to take its address for an FFI binding table. */
JCE_API void JCE_CALL jce_engine_set_app_desc_sized(const JceAppDesc *desc,
                                                    size_t desc_size);

/* Legacy entry point: assumes the caller's JceAppDesc matches the engine's.
   Kept so already-compiled binaries keep linking; new code should let the
   shim below pick the sized form. */
JCE_API void JCE_CALL jce_engine_set_app_desc(const JceAppDesc *desc);

#if !defined(JCE_BUILDING_ENGINE) && !defined(JCE_NO_APP_DESC_SIZE_SHIM)
#  define jce_engine_set_app_desc(desc) \
       jce_engine_set_app_desc_sized((desc), sizeof(JceAppDesc))
#endif

/* Set optional config file path override used by jce_engine_create.
   Pass NULL or empty string to clear override. */
JCE_API void JCE_CALL jce_engine_set_config_path(const char *path);

/* Set optional PAK file path for JNI desktop mode.
   Pass NULL or empty string to clear override.
   When set, jce_engine_create() loads the PAK from this file
   instead of embedding it in the binary. */
JCE_API void JCE_CALL jce_engine_set_pak_path(const char *path);

/* Override the renderer backend selection.
   Must be called before jce_engine_create().
   Pass JCE_BACKEND_AUTO (0) to clear override and use config file value.
   Requires jce_config.h for JceRendererBackend enum. */
JCE_API void JCE_CALL jce_engine_set_renderer_override(int backend);

/* Create the engine: init logger, load config, open PAK, create
   window/renderer/audio/input, async-load assets, create app. */
JCE_API JceEngine     *jce_engine_create(int argc, char *argv[]);

/* Route a platform event to input + app.
   Returns JCE_APP_SUCCESS on quit event.
   The event pointer is backend-specific (SDL_Event* internally). */
JCE_API JceAppResult JCE_CALL jce_engine_event(JceEngine *e, const void *event);

/* Run one frame: begin_frame, app_update, end_frame, input_update. */
JCE_API JceAppResult JCE_CALL jce_engine_iterate(JceEngine *e);

/* Shut down everything in reverse order. */
JCE_API void JCE_CALL jce_engine_destroy(JceEngine *e);

/* ---- FixedUpdate cadence (P3-B.2) ------------------------------ */

/* Configure the JCE_PHASE_FIXED_UPDATE rate driven by the default
 * fixed clock.  `hz` is in Hertz; the corresponding `fixed_dt` is
 * 1/hz.  Default: 60 Hz (== the runtime/physics step).  The per-runtime
 * physics clock adopts this cadence each step, so this knob also governs
 * physics (P1-fixed-clock-unify).  Pass <= 0 to restore the default.
 * Safe to call at any time; the change takes effect on the next
 * jce_engine_iterate / jce_runtime_step. */
JCE_API void   JCE_CALL jce_engine_set_fixed_hz(double hz);

/* Current FixedUpdate cadence in Hertz (1.0 / fixed_dt). */
JCE_API double JCE_CALL jce_engine_get_fixed_hz(void);

/* ── Frame-rate cap (Unity's Application.targetFrameRate) ─────────────
 *
 * The engine had NO frame limiter.  A shipped game sitting on a menu, or any
 * scene the GPU finishes early, ran the loop as fast as it could -- thousands
 * of frames a second on a simple scene -- burning a laptop's battery and
 * spinning its fans for frames nobody sees.  vsync bounds it only when vsync
 * is on AND the swapchain actually blocks; Project Settings > Quality has
 * carried a Target Framerate field all along, with nothing to hand it to.
 *
 * `fps <= 0` is uncapped (the default, and Unity's -1).  Otherwise
 * jce_engine_iterate waits at the end of each frame until the frame's share of
 * a second has passed: it sleeps the bulk and spins only the last fraction of
 * a millisecond, because a pure sleep overshoots on a 1 ms-granularity OS
 * timer and a pure spin would burn the CPU this exists to save.
 *
 * The deadline advances from the PREVIOUS deadline, not from "now", so the
 * cap does not drift late by the sleep's own overshoot every frame; a frame
 * that overran its budget resets the deadline instead of being repaid with a
 * burst of zero-length frames.
 *
 * Process-global, matching jce_engine_set_fixed_hz: a game has one loop, and
 * this must be callable before the engine exists.  Safe from any thread; the
 * wait itself happens on the loop thread. */
JCE_API void JCE_CALL jce_engine_set_target_fps(int fps);
JCE_API int  JCE_CALL jce_engine_get_target_fps(void);

/* ---- Optional scene-asset bundle catalog ----------------------- */

/* Set the bundle catalog path *before* jce_engine_create().  When set,
 * the engine opens the catalog after the legacy monolithic PAK and
 * keeps it accessible via jce_engine_get_bundle_catalog().  Game/editor
 * code can then mount/unmount per-scene bundles on demand. */
JCE_API void JCE_CALL jce_engine_set_bundle_catalog_path(const char *path);

/* Returns the active JceBundleCatalog* (opaque) or NULL.  Cast to
 * JceBundleCatalog* (declared in <jce/resource/jce_bundle_loader.h>). */
JCE_API void *JCE_CALL jce_engine_get_bundle_catalog(JceEngine *e);

/* ---- Action-map input (QW-input-actions) ----------------------- */

/* Returns the engine-owned JceInputActions, or NULL.  The engine loads
 * `.jce/input_actions.json` (editor-authored) at boot — falling back to
 * the built-in FPS defaults — and calls jce_actions_update() each frame,
 * so consumers can query logical actions (move_forward, jump, ...) via
 * <jce/os/platform/jce_input_actions.h> instead of raw scancodes.  The
 * same handle is also exposed through JceServices.actions. */
JCE_API JceInputActions *JCE_CALL jce_engine_get_actions(JceEngine *e);

/* ---- In-game graphics persistence (settings S7 follow-up) ------- */

/* Layer the jce.ini [graphics] section onto the live render pipeline:
 * named quality preset first, then the individual toggles — the same
 * precedence as the in-game settings screen's Apply.  Strict no-op when
 * the config carries no [graphics] section (cfg->gfx_valid false).  The
 * engine calls this right after the boot render-pipeline resolution;
 * jce_default_main calls it again after apply_boot_mounted re-resolves a
 * packed .rp.json, so the player's saved choices (layer 4, USER CONFIG)
 * stay on top of the asset (layer 3).  `renderer` may be NULL (skips the
 * live MSAA reset). */
JCE_API void JCE_CALL jce_engine_apply_graphics_config(
        const struct JceConfig *cfg, struct JceRenderer *renderer);

/* ---- Programmatic quit ------------------------------------------ */

/* Ask the main loop to exit cleanly on the next frame (same path as the
 * window close button, including the WILL_QUIT lifecycle event).  Safe to
 * call from anywhere on the main thread; used by the default_main runtime
 * presets (hold ESC 2s) and available to any app/driver code. */
JCE_API void JCE_CALL jce_engine_request_quit(void);
JCE_API bool JCE_CALL jce_engine_quit_requested(void);

/* Forget which backend AUTO settled on, so the next launch walks the platform
 * chain again from the most modern backend down to the most compatible one.
 *
 * This is what "decide again" means, and it needs to be an explicit act: a
 * host that merely passes AUTO on every launch is asking for the REMEMBERED
 * answer, not for the ladder to run each time.  Without this call there is no
 * way to ask for a re-decision short of hand-editing renderer.backend_resolved
 * out of the ini, which is not something a UI can offer.
 *
 * Reads and rewrites the engine config file wherever the engine would look for
 * it, so callers do not need the path.  Returns false if the file could not be
 * written (an ini on read-only media, say), in which case nothing changed.
 * Takes effect on the next launch; the live renderer is not touched. */
JCE_API bool JCE_CALL jce_engine_forget_resolved_backend(void);

JCE_EXTERN_C_END

#endif /* JCE_ENGINE_H */
