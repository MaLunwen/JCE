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

/* Optional same-interval host observations. CPU facts do not certify GPU
 * execution, presentation or successful pixels. Each availability bit is
 * independent; a retained allocation is not proof of fresh rendering. */
typedef enum JceFrameViewportCpuOutcome {
    JCE_FRAME_VIEWPORT_CPU_UNKNOWN = 0,
    JCE_FRAME_VIEWPORT_CPU_SKIPPED = 1,
    JCE_FRAME_VIEWPORT_CPU_FAILED = 2,
    JCE_FRAME_VIEWPORT_CPU_COMPLETED = 3
} JceFrameViewportCpuOutcome;

typedef enum JceFrameViewportObservationFlags {
    JCE_FRAME_VIEWPORT_REQUESTED_SIZE_VALID = 1u << 0,
    JCE_FRAME_VIEWPORT_ALLOCATED_SIZE_VALID = 1u << 1,
    JCE_FRAME_VIEWPORT_PREPARE_SUCCEEDED = 1u << 2,
    JCE_FRAME_VIEWPORT_DISPLAY_SIZE_VALID = 1u << 3,
    JCE_FRAME_VIEWPORT_TEXTURE_VALID = 1u << 4,
    JCE_FRAME_VIEWPORT_IMAGE_COUNT_VALID = 1u << 5,
    JCE_FRAME_VIEWPORT_QUEUE_COUNT_VALID = 1u << 6,
    JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID = 1u << 7
} JceFrameViewportObservationFlags;

typedef struct JceFrameViewportObservation {
    uint32_t struct_size;
    uint32_t source_id;
    uint32_t flags;
    uint32_t cpu_outcome;
    uint32_t refusal_reason;
    uint32_t requested_width, requested_height;
    uint32_t allocated_width, allocated_height;
    uint32_t display_width, display_height;
    uint32_t texture_index;
    uint32_t image_count;
    uint32_t queued_commands;
    uint32_t main_submits;
    uint32_t recording_submits;
} JceFrameViewportObservation;

/* Enabled only for explicit frame-context modes 2 and 3, while the current Engine's
 * main-thread app.draw callback is active. Off/v1 performs no collection.
 * Reports are copied immediately, at most once per interval, with 0..4 unique
 * nonzero caller-assigned source IDs. NULL/count0 is an explicit empty report;
 * no report remains unknown. No caller pointers survive this call.
 * A nonempty report requires a 4-byte aligned stride of 64..4096 bytes.
 * An empty report accepts zero or a valid stride. Each struct_size must be
 * 4-byte aligned, at least the known prefix and no larger than stride. Extra
 * trailing fields are ignored. Input addresses require uint32_t alignment.
 * Unavailable fields must be zero. Valid sizes must be nonzero pairs; a valid
 * texture index is 0..65534, and observed counts may be zero. Unknown flags,
 * outcomes, malformed input or duplicate reporting fail without changing a
 * previously accepted observation. A new interval clears the observation. */
JCE_API bool JCE_CALL jce_engine_frame_observation_enabled(void);
JCE_API bool JCE_CALL jce_engine_report_frame_observations(
    const JceFrameViewportObservation *observations, uint32_t count, size_t stride);

typedef struct JceEngine       JceEngine;
typedef struct JceAppDesc      JceAppDesc;
typedef struct JceInputActions JceInputActions;
struct JceConfig;
struct JceRenderer;

/* Register the application descriptor (IApp callbacks).
   Must be called on the host thread before engine creation. Descriptor and
   other pre-boot configuration setters refuse changes while any engine owner
   exists, including construction and deferred teardown. This preserves the
   original exit callback/user data until that owner's final release.
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

/* Create the engine with explicit cleanup ownership, on the host thread.
   owner must be a valid empty slot; an occupied slot is rejected unchanged.
   CONTINUE means fully initialized and ready. FAILURE can still leave *owner
   non-NULL when cleanup must be retried; retain it and call try_destroy until
   completion. A failed/closing owner admits no new event, update or draw work.
   Only one engine session may exist in a process, including pending cleanup.
   The slot is published before app.init, so callbacks can request its closure;
   closure during init forces failed creation even if init returns true. */
JCE_API JceAppResult JCE_CALL jce_engine_create_owned(int argc, char *argv[],
                                                     JceEngine **owner);

/* Legacy creation ABI: returns NULL on failure. If failed startup cannot yet
   release its owner, the engine retains one process-local pending owner.
   destroy(NULL) or a later legacy create retries it. That create invocation
   drains only and returns NULL even if cleanup completes; it never starts the
   retained application's descriptor again implicitly. Complete that retry before reapplying pre-boot
   settings for a replacement application; pending ownership rejects setters.
   Prefer create_owned to observe completion directly.
   Initializes logger/config/PAK, window/renderer/audio/input/assets and app. */
JCE_API JceEngine     *jce_engine_create(int argc, char *argv[]);

/* Route a platform event to input + app.
   Returns JCE_APP_SUCCESS on quit event.
   The event pointer is backend-specific (SDL_Event* internally). */
JCE_API JceAppResult JCE_CALL jce_engine_event(JceEngine *e, const void *event);

/* Run one frame: begin_frame, app_update, end_frame, input_update.
   Recursive entry during a frame or event callback returns CONTINUE. */
JCE_API JceAppResult JCE_CALL jce_engine_iterate(JceEngine *e);

/* Shut down owned services on the host thread. A NULL slot is rejected; an
   already empty slot succeeds. An address other than the current owner is
   rejected before dereference. The caller must discard all aliases after
   completion; raw pointers cannot distinguish a later reused address.
   Returns true and clears *owner only when teardown has finished. False leaves
   the slot unchanged for retry; constructing and active callbacks keep their
   borrowed services until the enclosing call returns. Closing blocks further
   application work. Native joins may have platform-specific latency; this is
   an ownership result, not a portable wall-clock deadline or GPU fence. */
JCE_API bool JCE_CALL jce_engine_try_destroy(JceEngine **owner);

/* Legacy shutdown ABI, using the same cleanup owner as try_destroy.
   NULL retries the pending owner from a failed legacy create, if any.
   During a frame or event callback, requests quit and retains the engine and
   its services until the outer event/iterate call returns. The host must call
   destroy again after that return, normally on a terminal application result.
   Shutdown also retains ownership if the shared async executor cannot stop
   inside an active callback; retry after the outer owner-thread pump returns.
   Terminal finalization and teardown reject recursive destruction. */
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

/* Effective background CPU permission for the single host, default false.
 * Main-thread only, callable before boot. The stock/Editor host policy publishes
 * authored run-in-background intent subject to its manual/OS pause, and restores
 * the prior value conditionally when that host closes. The getter observes this
 * published permission, not an author's original project setting. It permits
 * update-only ticks while minimized/non-drawable, never overrides OS PAUSE, and
 * does not enable rendering or alter foreground auxiliary-hook frequency. */
JCE_API void JCE_CALL jce_engine_set_run_in_background(bool enabled);
JCE_API bool JCE_CALL jce_engine_get_run_in_background(void);

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
/* Request terminal SUCCESS or FAILURE on the main thread, through the same
 * frame/KPI finalization, WILL_QUIT and host teardown path. FAILURE is sticky:
 * later successful/legacy quit requests cannot downgrade it. CONTINUE and
 * other invalid values are ignored. Requests before create are retained;
 * only fully completed engine destruction clears this session's request.
 * Deferred/reentrant destruction and early creation failures do not clear it.
 * Compatible additive API; legacy request_quit requests SUCCESS. */
JCE_API void JCE_CALL jce_engine_request_quit_with_result(JceAppResult result);
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
