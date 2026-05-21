/*
 * jce_scene_async.h  Async scene / prefab instantiate (P3-A.3).
 *
 * Unity parity: SceneManager.LoadSceneAsync(additive) + InstantiateAsync.
 *
 * Today the synchronous loaders (jce_scene_serial_load_*, jce_prefab_*)
 * block the main thread for the full I/O + parse + ECS-apply cost.  This
 * module splits that work:
 *
 *   - A background worker (jce_thread) opens the VFS file and runs the
 *     JSON parser.  This is by far the heaviest portion of a load.
 *   - The main thread calls jce_scene_async_dispatch_main() once per
 *     frame (wired into JCE_PHASE_EARLY_UPDATE by jce_scene_async_init);
 *     at most one parsed payload is committed to the live flecs world
 *     per call, so the per-frame spike is bounded even if several loads
 *     finish at once.
 *
 * The sync loaders are NOT removed — this API is purely additive.
 *
 * Threading:
 *   - jce_scene_instantiate_async / status / progress / get_result /
 *     cancel / in_flight_count are safe to call from the main thread.
 *   - The completion callback fires on the MAIN THREAD from inside
 *     jce_scene_async_dispatch_main().  Treat it like a Unity coroutine
 *     resume: you can freely touch the scene from there.
 *
 * Layer: Middleware/Scene (L4).  Reached via <jce/api_scene.h>.
 */

#ifndef JCE_SCENE_ASYNC_H
#define JCE_SCENE_ASYNC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene      JceScene;
typedef struct JceFileSystem JceFileSystem;

/* Opaque-ish handle.  0 == invalid (never returned for a live load). */
typedef uint32_t JceLoadHandle;
#define JCE_LOAD_HANDLE_INVALID ((JceLoadHandle)0)

typedef enum JceLoadStatus {
    JCE_LOAD_PENDING   = 0,  /* queued, worker not started yet           */
    JCE_LOAD_RUNNING,        /* worker parsing OR main thread applying   */
    JCE_LOAD_COMPLETE,       /* result is populated, callback fired      */
    JCE_LOAD_FAILED,         /* result.error_code != 0                   */
    JCE_LOAD_CANCELLED       /* aborted via jce_scene_async_cancel       */
} JceLoadStatus;

typedef enum JceLoadMode {
    JCE_LOAD_MODE_SINGLE   = 0, /* clear scene first (Unity Single)      */
    JCE_LOAD_MODE_ADDITIVE  = 1 /* append on top (Unity Additive)        */
} JceLoadMode;

typedef struct JceLoadResult {
    uint64_t root_entity;       /* first entity created (flecs id)       */
    uint32_t entity_count;      /* total entities instantiated           */
    uint32_t error_code;        /* 0 on success                          */
} JceLoadResult;

typedef void (*JceLoadCallback)(JceLoadHandle h, JceLoadStatus status,
                                const JceLoadResult *result, void *user);

/* ── Setup ──────────────────────────────────────────────────────────
 *
 * Called once at engine startup.  `target` is the scene every async load
 * commits into; `fs` is the VFS handle used to resolve paths (NULL ⇒
 * host filesystem fallback, like the sync loader).  Registers a callback
 * on JCE_PHASE_EARLY_UPDATE that drives jce_scene_async_dispatch_main().
 *
 * Safe to call multiple times — the latest call wins.  Returns false if
 * the registry could not be allocated. */
JCE_API bool JCE_CALL jce_scene_async_init(JceScene             *target,
                                            const JceFileSystem  *fs);

/* Shut down the async loader: joins any worker threads still running,
 * drops pending state, unregisters the player-loop hook.  Called by the
 * engine shutdown sequence; idempotent. */
JCE_API void JCE_CALL jce_scene_async_shutdown(void);

/* ── Submit ─────────────────────────────────────────────────────────
 *
 * Start an async load of `vfs_path` (a scene or prefab JSON, both share
 * the same envelope).  Returns JCE_LOAD_HANDLE_INVALID if the registry
 * is full or `vfs_path` is NULL.  `cb` may be NULL — callers can poll
 * status instead.  `user` is passed through to the callback verbatim. */
JCE_API JceLoadHandle JCE_CALL
jce_scene_instantiate_async(const char     *vfs_path,
                             JceLoadMode     mode,
                             JceLoadCallback cb,
                             void           *user);

/* ── Polling ────────────────────────────────────────────────────────
 *
 * status:  current state.  Returns JCE_LOAD_FAILED for unknown handles
 *          (stale, never submitted, or already reaped past the result
 *          retention window).
 * progress: 0.0 .. 1.0 with a parse/apply weighting (parse = 40 %,
 *          apply = 60 %).  Monotonic per handle.
 * get_result: fills `out` and returns true iff status is COMPLETE.
 *          After a COMPLETE handle is read once it is reaped on the
 *          next dispatch_main call; cache the result if needed. */
JCE_API JceLoadStatus JCE_CALL jce_scene_async_status(JceLoadHandle h);
JCE_API float         JCE_CALL jce_scene_async_progress(JceLoadHandle h);
JCE_API bool          JCE_CALL jce_scene_async_get_result(JceLoadHandle h,
                                                            JceLoadResult *out);

/* Cooperative cancel.  The worker checks at file-read and post-parse
 * boundaries; the main thread checks before applying.  A cancelled
 * handle ends up in JCE_LOAD_CANCELLED with no entities committed. */
JCE_API void          JCE_CALL jce_scene_async_cancel(JceLoadHandle h);

/* ── Driver ─────────────────────────────────────────────────────────
 *
 * Pumps state transitions on the main thread:
 *   - Promotes PENDING handles to RUNNING (spawns the worker).
 *   - For RUNNING handles whose worker is done parsing: commits at most
 *     ONE parsed payload to the scene this call, then fires the user
 *     callback.  Other RUNNING handles wait for the next call.
 *   - Reaps handles that were already COMPLETE / FAILED / CANCELLED on
 *     the previous tick.
 *
 * Engine-internal: registered automatically by jce_scene_async_init().
 * Game code should not call this directly. */
JCE_API void          JCE_CALL jce_scene_async_dispatch_main(void);

/* Number of non-reaped handles whose status is PENDING or RUNNING.
 * Editor / profiler convenience. */
JCE_API uint32_t      JCE_CALL jce_scene_async_in_flight_count(void);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_ASYNC_H */
