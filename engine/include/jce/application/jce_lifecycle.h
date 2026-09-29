/*
 * jce_lifecycle.h  Application lifecycle event registry (P3-B.3).
 *
 * Unity parity:
 *   FOCUS_GAINED / FOCUS_LOST   ≈ OnApplicationFocus
 *   PAUSE        / RESUME       ≈ OnApplicationPause
 *   LOW_MEMORY                  ≈ OnApplicationLowMemory
 *   WILL_QUIT                   ≈ OnApplicationQuit (last-chance to save)
 *   DEVICE_LOST  / DEVICE_RESET ≈ GPU device-lost notifications (desktop)
 *
 * Events are emitted by the platform layer (SDL3 event bridge) on the
 * MAIN thread.  Callbacks must therefore not assume any other thread
 * affinity, and must be cheap (no blocking I/O — save snapshots should
 * be queued, not synchronous).
 *
 * Why this is not built on jce_event_bus (os/core), even though L6 may
 * depend on L2 — four contracts that bus cannot express:
 *   1. Priority order.  The bus dispatches in registration order; this
 *      registry sorts ascending by priority with stable ties.  That is
 *      load-bearing: the engine registers its own listener at -1000 so
 *      engine-internal teardown runs ahead of consumer listeners.
 *   2. Removal.  The bus removes by (fn, userdata) with a swap-to-last
 *      that destroys ordering; this removes by opaque id via memmove
 *      and keeps the sort intact.
 *   3. Duplicates.  The bus silently drops a repeat (fn, userdata);
 *      here the same callback may be registered at two priorities and
 *      gets a distinct handle for each.
 *   4. Lifetime.  The bus is a caller-owned instance; jce_engine_destroy
 *      frees it well before it calls jce_lifecycle_shutdown(), and emit
 *      must stay usable in between.  Being a process-global with no
 *      handle to thread through is the point, not an accident.
 *
 * Relationship to the JceEvent stream (jce_window_event.h): focus and
 * quit surface on both channels, and they are NOT the same fact.
 *   - JCE_EVENT_WINDOW_FOCUS_* / JCE_EVENT_QUIT / JCE_EVENT_WINDOW_CLOSE
 *     go to the single JceAppDesc::on_event callback.  Quit there is a
 *     *request*: it is delivered before should_quit() arbitration and is
 *     still cancellable (returning false swallows it and the app runs on).
 *   - JCE_LIFECYCLE_* is the multi-listener broadcast.  WILL_QUIT is the
 *     committed decision to exit — emitted only on paths that really do
 *     terminate, including paths that produce no JceEvent at all
 *     (SDL_EVENT_TERMINATING, jce_engine_quit_requested(), JCE_MAX_FRAMES).
 * So for "are we shutting down / focused" this header is authoritative;
 * on_event is the raw request stream.  Neither channel is redundant —
 * do not remove one to feed the other.
 *
 * Layer: L6 (application).  Consumed via <jce/api_app.h>.
 */

#ifndef JCE_LIFECYCLE_H
#define JCE_LIFECYCLE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum JceLifecycleEvent {
    JCE_LIFECYCLE_FOCUS_GAINED = 0,
    JCE_LIFECYCLE_FOCUS_LOST,
    JCE_LIFECYCLE_PAUSE,         /* app backgrounded (mobile / minimized desktop) */
    JCE_LIFECYCLE_RESUME,        /* app foregrounded again                        */
    JCE_LIFECYCLE_LOW_MEMORY,    /* OS signals memory pressure                    */
    JCE_LIFECYCLE_WILL_QUIT,     /* about to exit — last chance to save           */
    JCE_LIFECYCLE_DEVICE_LOST,   /* GPU device lost (desktop only typically)      */
    JCE_LIFECYCLE_DEVICE_RESET,  /* GPU device reset complete                     */
    JCE_LIFECYCLE_EVENT_COUNT
} JceLifecycleEvent;

typedef void (JCE_CALL *JceLifecycleCallback)(JceLifecycleEvent event, void *user);

/* Opaque handle returned by registration.  id == 0 means invalid. */
typedef struct JceLifecycleHandle {
    uint32_t id;
} JceLifecycleHandle;

/* Register a callback.  Lower priority runs earlier.  Returns a handle
 * whose `id` is non-zero on success.  Main-thread only. */
JCE_API JceLifecycleHandle JCE_CALL
jce_lifecycle_register(JceLifecycleCallback cb, int32_t priority, void *user);

JCE_API void JCE_CALL jce_lifecycle_unregister(JceLifecycleHandle h);

/* Emit an event to all listeners (in priority order).  This is the
 * platform-internal hook called by the SDL event pump; game code
 * should not call it directly.  MAIN THREAD ONLY -- see the note at the
 * top of this header: every listener is written on that assumption. */
JCE_API void JCE_CALL jce_lifecycle_emit(JceLifecycleEvent event);

/* WHERE DEVICE_LOST COMES FROM, because it came from nowhere until
 * 2026-09-21 and the next reader will want to know.
 *
 * JCE_LIFECYCLE_DEVICE_LOST had ZERO emitters: the whole tracked tree named
 * it four times -- the enum above, its case in jce_lifecycle_event_to_string(),
 * its row in contracts/abi-snapshot.txt, and a platform AGENTS.md table
 * saying "(no SDL counterpart yet); renderer may emit manually".  A
 * consumer's jce_lifecycle_register(DEVICE_LOST, ...) succeeded and the
 * callback was dead code, with nothing anywhere saying so.
 *
 * THE OBVIOUS FIX WAS WRONG.  SDL_EVENT_RENDER_DEVICE_LOST does exist in
 * SDL3, so the table's "no SDL counterpart" is stale -- but it is an
 * SDL_Render event, raised for an SDL_Renderer, and this engine creates one
 * ONLY in the safe-mode software fallback (jce_renderer.c: "Force the pure
 * CPU software renderer").  Translating it beside the DEVICE_RESET case
 * would have fired on the diagnostic path and never on a real backend --
 * the same defect wearing a green count.  IT ALSO MEANS THE EXISTING
 * DEVICE_RESET EMITTER IS DEAD on the normal path, for the same reason, and
 * that is recorded rather than quietly fixed: making it fire is a separate
 * decision about what "reset" means for a bgfx swap chain.
 *
 * The real signal is BGFX_FATAL_DEVICE_LOST.  The renderer records it and
 * jce_engine_iterate emits here on the main thread -- the renderer cannot
 * emit it itself, because <jce/application/...> is a layer it may not
 * include, and because bgfx delivers that callback on its render thread
 * everywhere except macOS. */

/* Convenience state getters reflecting the last paired event seen. */
JCE_API bool JCE_CALL jce_lifecycle_is_focused(void);
JCE_API bool JCE_CALL jce_lifecycle_is_paused(void);

/* String helper for logs.  Returns a static literal; never NULL. */
JCE_API const char *JCE_CALL jce_lifecycle_event_to_string(JceLifecycleEvent event);

/* Release the listener table.  Called from jce_engine_destroy after
 * every subsystem has had a chance to unregister.  Safe to call when
 * empty. */
JCE_API void JCE_CALL jce_lifecycle_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_LIFECYCLE_H */
