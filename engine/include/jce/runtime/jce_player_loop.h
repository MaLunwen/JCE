/*
 * jce_player_loop.h  Ordered, pluggable per-frame update phases (P3-B.1).
 *
 * Unity-parity: equivalent to UnityEngine.LowLevel.PlayerLoop.  Engine
 * subsystems and game code register callbacks against one of the 8
 * canonical phases, each with a numeric priority (lower = earlier).
 * The application orchestrator (jce_engine_iterate) walks the phases in
 * order every frame and invokes the registered callbacks.
 *
 * Threading: registration / unregistration are MAIN-THREAD ONLY in v1.
 * Phase dispatch is single-threaded.  A future revision may add a
 * thread-safe queueing layer; do not rely on it now.
 *
 * Layer: L5 (runtime).  Consumed via <jce/api_runtime.h>.
 */

#ifndef JCE_PLAYER_LOOP_H
#define JCE_PLAYER_LOOP_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Canonical per-frame phases.  Order is significant. */
typedef enum JcePlayerLoopPhase {
    JCE_PHASE_INITIALIZATION = 0, /* one-shot or per-frame setup work       */
    JCE_PHASE_EARLY_UPDATE,       /* input poll, event drain                */
    JCE_PHASE_FIXED_UPDATE,       /* deterministic step (P3-B.2: 0..N times)*/
    JCE_PHASE_UPDATE,             /* gameplay / ECS world tick              */
    JCE_PHASE_LATE_UPDATE,        /* post-gameplay (cameras, IK, anim post) */
    JCE_PHASE_PRE_RENDER,         /* renderer begin-frame, frame setup      */
    JCE_PHASE_POST_RENDER,        /* renderer submit / end-frame            */
    JCE_PHASE_END_OF_FRAME,       /* present, frame mark, swap-edge work    */
    JCE_PHASE_COUNT
} JcePlayerLoopPhase;

typedef void (*JcePlayerLoopFn)(float dt, void *user);

/* Opaque handle returned by registration; pass to unregister.
 * id == 0 means "invalid / not registered". */
typedef struct JcePlayerLoopHandle {
    uint32_t id;
} JcePlayerLoopHandle;

/* Register `fn` to run in `phase` at the given `priority` (lower runs
 * earlier; ties broken by registration order).  Returns a handle whose
 * `id` is non-zero on success.  Main thread only. */
JCE_API JcePlayerLoopHandle JCE_CALL
jce_player_loop_register(JcePlayerLoopPhase phase,
                         int32_t            priority,
                         JcePlayerLoopFn    fn,
                         void              *user);

/* Remove a previously registered callback.  No-op for invalid handles
 * or after a callback has already been unregistered. */
JCE_API void JCE_CALL
jce_player_loop_unregister(JcePlayerLoopHandle h);

/* Dispatch every callback registered in `phase`, in priority order.
 * Engine-internal: called by jce_engine_iterate.  Game code should
 * register callbacks rather than calling this directly. */
JCE_API void JCE_CALL
jce_player_loop_run_phase(JcePlayerLoopPhase phase, float dt);

/* Number of callbacks currently registered in `phase`.  Used by the
 * editor's Systems panel (P3-B.5) for diagnostics. */
JCE_API uint32_t JCE_CALL
jce_player_loop_phase_count(JcePlayerLoopPhase phase);

/* ── Introspection (P3-B.5 Systems panel) ──────────────────────────
 *
 * `jce_player_loop_iterate` walks every registered callback (across all
 * phases, in dispatch order) and invokes `cb`.  The struct passed to
 * `cb` is a *snapshot*: callers must not retain pointers past the
 * callback return.  Designed for read-only inspection by the editor.
 *
 * `set_enabled` lets the editor temporarily skip a callback at dispatch
 * time without unregistering it.  `set_debug_name` attaches a short
 * human-readable label (up to 31 chars + NUL) shown in the panel; the
 * string is copied into a per-entry buffer, no lifetime constraints on
 * the caller's pointer.
 */

typedef struct JcePlayerLoopEntry {
    JcePlayerLoopPhase phase;
    int32_t            priority;
    const char        *debug_name;     /* NULL if not set */
    void              *fn;              /* function pointer (opaque) */
    void              *user;
    uint32_t           id;
    double             last_ms;         /* most recent call duration (ms) */
    bool               enabled;         /* false = skipped at dispatch */
} JcePlayerLoopEntry;

typedef void (*JcePlayerLoopIterFn)(const JcePlayerLoopEntry *entry,
                                    void *user);

JCE_API void JCE_CALL
jce_player_loop_iterate(JcePlayerLoopIterFn cb, void *user);

JCE_API void JCE_CALL
jce_player_loop_set_enabled(uint32_t id, bool enabled);

JCE_API void JCE_CALL
jce_player_loop_set_debug_name(uint32_t id, const char *name);

/* Maps a phase enum to a stable English identifier (used as i18n key
 * suffix and panel-internal label).  Returns "unknown" for out-of-range
 * input. */
JCE_API const char *JCE_CALL
jce_player_loop_phase_to_string(JcePlayerLoopPhase phase);

/* Tear down all phase storage.  Called by the engine on shutdown. */
JCE_API void JCE_CALL
jce_player_loop_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_PLAYER_LOOP_H */
