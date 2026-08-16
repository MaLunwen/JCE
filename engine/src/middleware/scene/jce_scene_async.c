/*
 * jce_scene_async.c  Async scene / prefab instantiate (P3-A.3).
 *
 * Design rationale (see jce_scene_async.h for the public contract):
 *
 *  - A small fixed slot table (JCE_ASYNC_SLOTS) holds all in-flight
 *    loads.  Handles encode a (generation, index) pair so stale handles
 *    are detectable after a slot is reused.
 *  - I/O + JSON parse uses the process structured executor. This bounds
 *    worker count, supplies cancellation/backpressure, and preserves
 *    deferred cooperative execution on Web without pthreads.
 *  - Apply-to-flecs happens on the main thread.  flecs is not safe for
 *    concurrent entity creation against a live world without manual
 *    deferral, and the existing jce_scene_load_json() routine is a
 *    two-pass operation (create + parent-fixup) that doesn't slice
 *    cleanly.  We therefore commit at most ONE finished load per
 *    dispatch_main call to bound per-frame spikes.
 *  - Progress is published by the task context at parse completion and
 *    advanced by the main thread until apply lands.
 *  - Cancellation is cooperative at file-read, post-parse, and pre-apply
 *    boundaries.
 */

#include <jce/middleware/scene/jce_scene_async.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_timer.h>   /* monotonic clock for the parse ramp */
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_thread.h>
#include <jce/resource/jce_scene_serial.h>
#include <jce/runtime/jce_player_loop.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "scene_async"

#define JCE_ASYNC_SLOTS 64

/* Internal phase distinct from the public JceLoadStatus so we can
 * distinguish "worker still parsing" from "ready for main-thread apply". */
typedef enum {
    SLOT_FREE = 0,
    SLOT_PENDING,        /* registered, worker not started yet         */
    SLOT_PARSING,        /* worker reading file + parsing JSON         */
    SLOT_PARSED,         /* JSON tree ready, awaiting main-thread apply*/
    SLOT_APPLIED,        /* committed to scene, callback already fired */
    SLOT_FAILED_INT,     /* internal failure (I/O, parse, OOM)         */
    SLOT_CANCELLED_INT   /* cancel observed                            */
} SlotPhase;

typedef struct {
    /* Identity. */
    uint16_t       generation;     /* bumped each time the slot is reused */
    uint8_t        in_use;         /* 1 if owned by a load                */
    uint8_t        reap_pending;   /* 1 if next dispatch should free      */

    /* Input. */
    char          *vfs_path;       /* JCE_MALLOC'd copy                   */
    JceLoadMode    mode;
    JceLoadCallback cb;
    void          *user;
    const JceFileSystem *fs;
    bool           cancel_requested;

    /* Worker handoff. */
    JceAsyncTask *task;
    SlotPhase      phase;          /* main-thread visible phase           */

    /* Parsed payload (produced by worker, consumed by main). */
    JceJson       *parsed_root;    /* NULL if parse failed                */
    uint32_t       parsed_entity_count;
    uint32_t       worker_error;   /* non-zero on parse/IO failure        */

    /* Output. */
    JceLoadResult  result;
    JceLoadStatus  public_status;  /* what callers see via _status()      */
    float          progress;       /* 0..1, monotonic per slot            */
    uint64_t       parse_started_ms; /* monotonic stamp for the parse ramp */
} AsyncSlot;

/* ── Module state ──────────────────────────────────────────────────── */

static AsyncSlot        g_slots[JCE_ASYNC_SLOTS];
static JceMutex        *g_mu               = NULL;   /* guards g_slots */
static JceScene        *g_target_scene     = NULL;
static const JceFileSystem *g_target_fs    = NULL;
static JcePlayerLoopHandle g_loop_handle   = {0};
static bool             g_initialised      = false;

/* ── Handle encoding ──────────────────────────────────────────────── */

static JceLoadHandle handle_make(uint16_t idx, uint16_t gen)
{
    /* idx in [0..JCE_ASYNC_SLOTS); slot 0 is reserved so a fully-zero
     * encoding maps to INVALID — callers can safely test against the
     * macro without juggling sentinels. */
    return ((uint32_t)(gen + 1u) << 16) | (uint32_t)(idx + 1u);
}

static bool handle_decode(JceLoadHandle h, uint16_t *idx, uint16_t *gen)
{
    if (h == JCE_LOAD_HANDLE_INVALID) return false;
    uint32_t raw_idx = (h & 0xFFFFu);
    uint32_t raw_gen = (h >> 16);
    if (raw_idx == 0 || raw_gen == 0) return false;
    if (raw_idx - 1u >= JCE_ASYNC_SLOTS) return false;
    *idx = (uint16_t)(raw_idx - 1u);
    *gen = (uint16_t)(raw_gen - 1u);
    return true;
}

/* Caller must hold g_mu.  Returns NULL for stale or freed handles. */
static AsyncSlot *slot_lookup_locked(JceLoadHandle h)
{
    uint16_t idx = 0, gen = 0;
    if (!handle_decode(h, &idx, &gen)) return NULL;
    AsyncSlot *s = &g_slots[idx];
    if (!s->in_use || s->generation != gen) return NULL;
    return s;
}

/* ── Slot lifecycle ───────────────────────────────────────────────── */

static void slot_free_payload(AsyncSlot *s)
{
    if (s->parsed_root) {
        jce_json_free(s->parsed_root);
        s->parsed_root = NULL;
    }
    if (s->vfs_path) {
        JCE_FREE(s->vfs_path);
        s->vfs_path = NULL;
    }
}

static void slot_reap(AsyncSlot *s)
{
    if (s->task) {
        (void)jce_async_task_cancel(s->task);
        jce_async_task_wait(s->task);
        jce_async_task_release(s->task);
        s->task = NULL;
    }
    slot_free_payload(s);
    uint16_t next_gen = (uint16_t)(s->generation + 1u);
    memset(s, 0, sizeof(*s));
    s->generation = next_gen;
}

/* ── Worker entry ─────────────────────────────────────────────────── */

typedef struct {
    AsyncSlot          *slot;
    const JceFileSystem *fs;
} WorkerArg;

static void worker_arg_cleanup(void *arg)
{
    JCE_FREE(arg);
}

static JceAsyncRunResult worker_main(JceAsyncContext *ctx, void *arg)
{
    WorkerArg *wa = (WorkerArg *)arg;
    AsyncSlot *s  = wa->slot;

    /* vfs_path is stable until the terminal task is reaped. */
    const char *path = s->vfs_path;

    if (jce_async_context_cancel_requested(ctx)) {
        s->worker_error = 0;
        return JCE_ASYNC_RUN_CANCELLED;
    }

    uint64_t size = 0;
    void    *buf  = NULL;
    if (wa->fs) {
        buf = jce_fs_read_all(wa->fs, path, &size);
    } else {
        buf = jce_fs_host_read_all(path, &size);
    }

    if (!buf || size == 0) {
        if (buf) JCE_FREE(buf);
        LOG_ERROR(LOG_TAG, "cannot read '%s'", path ? path : "(null)");
        s->worker_error = 1; /* I/O */
        jce_async_context_fail(ctx, 1, "scene read failed");
        return JCE_ASYNC_RUN_FAILED;
    }

    /* P3-A.5: account the staging buffer against the SCENE_ECS tag for
     * the brief window between read and parse. Freed below regardless
     * of the parse outcome. */
    jce_mem_profile_record_alloc(JCE_MEM_TAG_SCENE_ECS, (size_t)size);

    if (jce_async_context_cancel_requested(ctx)) {
        JCE_FREE(buf);
        jce_mem_profile_record_free(JCE_MEM_TAG_SCENE_ECS, (size_t)size);
        return JCE_ASYNC_RUN_CANCELLED;
    }

    JceJson *root = jce_json_parse((const char *)buf, (size_t)size);
    JCE_FREE(buf);
    jce_mem_profile_record_free(JCE_MEM_TAG_SCENE_ECS, (size_t)size);

    if (!root) {
        LOG_ERROR(LOG_TAG, "JSON parse failed for '%s'", path);
        s->worker_error = 2; /* parse */
        jce_async_context_fail(ctx, 2, "scene JSON parse failed");
        return JCE_ASYNC_RUN_FAILED;
    }

    /* Best-effort entity count for progress weighting. */
    uint32_t ecount = 0;
    {
        JceJson *e = jce_json_get(root, "entities");
        if (!e) {
            JceJson *scene = jce_json_get(root, "scene");
            if (scene) e = jce_json_get(scene, "entities");
        }
        if (e && jce_json_is_array(e)) {
            int n = jce_json_array_size(e);
            if (n > 0) ecount = (uint32_t)n;
        }
    }

    s->parsed_root          = root;
    s->parsed_entity_count  = ecount;
    s->worker_error         = 0;
    jce_async_context_set_progress(ctx, 0.4f);
    /* From this point the main thread owns parsed_root. */
    return jce_async_context_cancel_requested(ctx)
        ? JCE_ASYNC_RUN_CANCELLED : JCE_ASYNC_RUN_SUCCESS;
}

/* ── Player-loop tick ─────────────────────────────────────────────── */

static void count_cb_main(JceScene *sc, JceEntity e, void *ud)
{
    (void)sc; (void)e;
    *(uint32_t *)ud += 1u;
}

static void apply_one_locked(AsyncSlot *s)
{
    /* Pre-conditions: holding g_mu, phase == SLOT_PARSED. */
    if (s->cancel_requested) {
        s->phase         = SLOT_CANCELLED_INT;
        s->public_status = JCE_LOAD_CANCELLED;
        s->progress      = 1.0f;
        s->reap_pending  = 1;
        return;
    }

    if (!g_target_scene) {
        LOG_ERROR(LOG_TAG, "no target scene; call jce_scene_async_init() first");
        s->phase             = SLOT_FAILED_INT;
        s->public_status     = JCE_LOAD_FAILED;
        s->result.error_code = 3; /* no target */
        s->progress          = 1.0f;
        s->reap_pending      = 1;
        return;
    }

    if (s->mode == JCE_LOAD_MODE_SINGLE) {
        (void)jce_scene_clear(g_target_scene);
    }

    /* We reuse the existing jce_scene_load_json() — the same helper the
     * synchronous loader invokes — so parsing logic stays single-sourced.
     * To derive the new-entity count without exposing a fresh API we
     * tally the scene size before and after the apply. */
    uint32_t before = 0;
    uint32_t after  = 0;
    jce_scene_each_entity(g_target_scene, count_cb_main, &before);

    int n = s->fs
        ? jce_scene_serial_apply_json_vfs(g_target_scene, s->fs,
                                          s->vfs_path, s->parsed_root)
        : jce_scene_load_json(g_target_scene, s->parsed_root);
    if (n < 0) {
        LOG_ERROR(LOG_TAG, "scene apply failed for '%s'",
                  s->vfs_path ? s->vfs_path : "(null)");
        s->phase             = SLOT_FAILED_INT;
        s->public_status     = JCE_LOAD_FAILED;
        s->result.error_code = 4; /* apply */
        s->progress          = 1.0f;
        s->reap_pending      = 1;
        return;
    }

    jce_scene_each_entity(g_target_scene, count_cb_main, &after);

    s->result.entity_count = (after > before) ? (after - before)
                                              : (uint32_t)n;
    /* Root entity is unknown without changes to jce_scene_load_json;
     * 0 means "use the sync prefab API if you need the root handle". */
    s->result.root_entity  = 0;
    s->result.error_code   = 0;

    s->phase         = SLOT_APPLIED;
    s->public_status = JCE_LOAD_COMPLETE;
    s->progress      = 1.0f;
    s->reap_pending  = 1;

    LOG_SUCCESS(LOG_TAG, "async load '%s' done: %u entities",
                s->vfs_path ? s->vfs_path : "(null)", s->result.entity_count);
}

JCE_API void JCE_CALL jce_scene_async_dispatch_main(void)
{
    if (!g_initialised) return;

    jce_mutex_lock(g_mu);

    /* Stage 1: reap anything marked from a previous tick. */
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (s->in_use && s->reap_pending == 2) {
            /* Move public status into a non-readable state by reaping;
             * callers that wanted the result should have grabbed it via
             * get_result() before the next tick. */
            slot_reap(s);
        }
    }

    /* Stage 2: promote PENDING -> PARSING (submit bounded parse tasks). */
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        JceAsyncTaskDesc desc;
        JceAsyncExecutor *executor;
        if (!s->in_use || s->phase != SLOT_PENDING) continue;

        WorkerArg *wa = (WorkerArg *)JCE_CALLOC(1, sizeof(*wa));
        if (!wa) {
            s->phase             = SLOT_FAILED_INT;
            s->public_status     = JCE_LOAD_FAILED;
            s->result.error_code = 5; /* OOM */
            s->progress          = 1.0f;
            s->reap_pending      = 1;
            continue;
        }
        wa->slot = s;
        wa->fs = s->fs;
        executor = jce_async_default_executor();
        jce_async_task_desc_init(&desc);
        desc.work = worker_main;
        desc.cleanup = worker_arg_cleanup;
        desc.user_data = wa;
        desc.debug_name = s->vfs_path;
        desc.priority = JCE_ASYNC_PRIORITY_HIGH;
        s->task = executor ? jce_async_submit(executor, &desc) : NULL;
        if (!s->task) {
            JCE_FREE(wa);
            s->phase             = SLOT_FAILED_INT;
            s->public_status     = JCE_LOAD_FAILED;
            s->result.error_code = 6; /* executor unavailable/rejected */
            s->progress          = 1.0f;
            s->reap_pending      = 1;
            continue;
        }
        s->phase = SLOT_PARSING;
        s->public_status = JCE_LOAD_RUNNING;
        s->parse_started_ms = jce_time_ticks_ms();
    }

    /* Stage 3: harvest terminal parse tasks -> parsed/failed/cancelled. */
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        JceAsyncState task_state;
        if (!s->in_use || s->phase != SLOT_PARSING) continue;
        if (!s->task || !jce_async_task_is_terminal(s->task)) continue;

        task_state = jce_async_task_state(s->task);
        jce_async_task_release(s->task);
        s->task = NULL;
        if (task_state == JCE_ASYNC_STATE_CANCELLED) {
            s->phase         = SLOT_CANCELLED_INT;
            s->public_status = JCE_LOAD_CANCELLED;
            s->progress      = 1.0f;
            s->reap_pending  = 1;
            continue;
        }
        if (task_state == JCE_ASYNC_STATE_FAILED ||
            s->worker_error != 0 || !s->parsed_root) {
            s->phase             = SLOT_FAILED_INT;
            s->public_status     = JCE_LOAD_FAILED;
            s->result.error_code = s->worker_error ? s->worker_error : 7;
            s->progress          = 1.0f;
            s->reap_pending      = 1;
            continue;
        }
        s->phase    = SLOT_PARSED;
        s->progress = 0.4f; /* parse weight */
    }

    /* Stage 4: apply at most ONE parsed payload this frame. */
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (!s->in_use || s->phase != SLOT_PARSED) continue;
        apply_one_locked(s);
        break;
    }

    /* Stage 5: fire user callbacks for terminal states.  We do this
     * after unlocking so user code can re-enter the API safely. */
    typedef struct {
        JceLoadHandle   h;
        JceLoadStatus   status;
        JceLoadResult   result;
        JceLoadCallback cb;
        void           *user;
    } PendingCb;

    PendingCb pending[JCE_ASYNC_SLOTS];
    uint32_t  npending = 0;

    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (!s->in_use || !s->reap_pending) continue;
        if (s->public_status != JCE_LOAD_COMPLETE
         && s->public_status != JCE_LOAD_FAILED
         && s->public_status != JCE_LOAD_CANCELLED) continue;

        if (s->cb) {
            pending[npending].h =
                handle_make((uint16_t)i, s->generation);
            pending[npending].status = s->public_status;
            pending[npending].result = s->result;
            pending[npending].cb     = s->cb;
            pending[npending].user   = s->user;
            npending++;
            s->cb = NULL;
        }
        s->reap_pending = 2;
    }

    jce_mutex_unlock(g_mu);

    for (uint32_t i = 0; i < npending; i++) {
        pending[i].cb(pending[i].h, pending[i].status,
                      &pending[i].result, pending[i].user);
    }
}

/* ── Init / shutdown ──────────────────────────────────────────────── */

static void player_loop_tick(float dt, void *user)
{
    (void)dt; (void)user;
    jce_scene_async_dispatch_main();
}

JCE_API bool JCE_CALL jce_scene_async_init(JceScene             *target,
                                            const JceFileSystem  *fs)
{
    if (!g_initialised) {
        g_mu = jce_mutex_create();
        if (!g_mu) return false;
        memset(g_slots, 0, sizeof(g_slots));
        g_initialised = true;
    }
    jce_mutex_lock(g_mu);
    g_target_scene = target;
    g_target_fs    = fs;
    jce_mutex_unlock(g_mu);

    if (g_loop_handle.id == 0) {
        g_loop_handle = jce_player_loop_register(JCE_PHASE_EARLY_UPDATE,
                                                  /*priority=*/100,
                                                  player_loop_tick, NULL);
    }
    LOG_INFO(LOG_TAG, "initialised (target=%p, fs=%p)",
             (void *)target, (const void *)fs);
    return true;
}

JCE_API void JCE_CALL jce_scene_async_shutdown(void)
{
    if (!g_initialised) return;

    if (g_loop_handle.id != 0) {
        jce_player_loop_unregister(g_loop_handle);
        g_loop_handle.id = 0;
    }

    /* Signal cancellation on every live parse task. */
    jce_mutex_lock(g_mu);
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (!s->in_use) continue;
        s->cancel_requested = true;
        if (s->task)
            (void)jce_async_task_cancel(s->task);
    }
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (s->in_use) slot_reap(s);
    }
    jce_mutex_unlock(g_mu);

    jce_mutex_destroy(g_mu);
    g_mu             = NULL;
    g_target_scene   = NULL;
    g_target_fs      = NULL;
    g_initialised    = false;
}

/* ── Submit ───────────────────────────────────────────────────────── */

JCE_API JceLoadHandle JCE_CALL
jce_scene_instantiate_async(const char     *vfs_path,
                             JceLoadMode     mode,
                             JceLoadCallback cb,
                             void           *user)
{
    if (!g_initialised || !vfs_path) return JCE_LOAD_HANDLE_INVALID;

    jce_mutex_lock(g_mu);

    uint32_t slot_idx = JCE_ASYNC_SLOTS;
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        if (!g_slots[i].in_use) { slot_idx = i; break; }
    }
    if (slot_idx >= JCE_ASYNC_SLOTS) {
        jce_mutex_unlock(g_mu);
        LOG_WARN(LOG_TAG, "registry full (%d) — dropping '%s'",
                 JCE_ASYNC_SLOTS, vfs_path);
        return JCE_LOAD_HANDLE_INVALID;
    }

    AsyncSlot *s   = &g_slots[slot_idx];
    uint16_t   gen = s->generation;
    memset(s, 0, sizeof(*s));
    s->generation    = gen;
    s->in_use        = 1;
    s->mode          = mode;
    s->cb            = cb;
    s->user          = user;
    s->fs            = g_target_fs;
    s->phase         = SLOT_PENDING;
    s->public_status = JCE_LOAD_PENDING;
    s->progress      = 0.0f;

    size_t pl = strlen(vfs_path);
    s->vfs_path = (char *)JCE_MALLOC(pl + 1u);
    if (s->vfs_path) memcpy(s->vfs_path, vfs_path, pl + 1u);

    if (!s->vfs_path) {
        slot_free_payload(s);
        memset(s, 0, sizeof(*s));
        s->generation = (uint16_t)(gen + 1u);
        jce_mutex_unlock(g_mu);
        return JCE_LOAD_HANDLE_INVALID;
    }

    JceLoadHandle h = handle_make((uint16_t)slot_idx, gen);
    jce_mutex_unlock(g_mu);
    return h;
}

/* ── Polling ──────────────────────────────────────────────────────── */

JCE_API JceLoadStatus JCE_CALL jce_scene_async_status(JceLoadHandle h)
{
    if (!g_initialised) return JCE_LOAD_FAILED;
    jce_mutex_lock(g_mu);
    AsyncSlot *s = slot_lookup_locked(h);
    JceLoadStatus st = s ? s->public_status : JCE_LOAD_FAILED;
    jce_mutex_unlock(g_mu);
    return st;
}

/* Parse-phase progress ramp, factored out so it is testable without a live
 * worker thread.  Time-based and monotonic: `elapsed_ms` is measured from the
 * moment the slot entered SLOT_PARSING, NOT accumulated per call.  The old
 * form added a fixed step inside this getter, so progress tracked how often a
 * caller polled rather than how long the load had taken -- two panels reading
 * the same handle ran the bar at double speed, and a caller that stopped
 * polling froze it. */
JCE_API float JCE_CALL jce_async_parse_ramp(float current, uint64_t elapsed_ms)
{
    const float elapsed_s = (float)elapsed_ms * 0.001f;
    const float ramped =
        elapsed_s * (JCE_ASYNC_PARSE_CEILING / JCE_ASYNC_PARSE_RAMP_SECONDS);
    float next = current > ramped ? current : ramped;   /* never goes back */
    if (next > JCE_ASYNC_PARSE_CEILING) next = JCE_ASYNC_PARSE_CEILING;
    return next;
}

JCE_API float JCE_CALL jce_scene_async_progress(JceLoadHandle h)
{
    if (!g_initialised) return 0.0f;
    jce_mutex_lock(g_mu);
    AsyncSlot *s = slot_lookup_locked(h);
    float p = 0.0f;
    if (s) {
        /* Smoothly advance progress while the worker runs so callers see
         * motion before the parse boundary lands.  Driven by the monotonic
         * clock, NOT by a per-call increment: this is a getter, so the old
         * form advanced once per CALL -- a caller polling twice a frame ran
         * the bar twice as fast, and one that stopped polling froze it.  The
         * ramp now takes the same 1.75 s of wall time regardless of frame
         * rate or polling pattern. */
        if (s->phase == SLOT_PARSING && s->progress < JCE_ASYNC_PARSE_CEILING) {
            const uint64_t now_ms = jce_time_ticks_ms();
            if (s->parse_started_ms == 0) s->parse_started_ms = now_ms;
            s->progress = jce_async_parse_ramp(s->progress,
                                               now_ms - s->parse_started_ms);
        }
        p = s->progress;
    }
    jce_mutex_unlock(g_mu);
    return p;
}

JCE_API bool JCE_CALL jce_scene_async_get_result(JceLoadHandle h,
                                                  JceLoadResult *out)
{
    if (!g_initialised || !out) return false;
    jce_mutex_lock(g_mu);
    AsyncSlot *s = slot_lookup_locked(h);
    bool ok = false;
    if (s && s->public_status == JCE_LOAD_COMPLETE) {
        *out = s->result;
        ok = true;
    }
    jce_mutex_unlock(g_mu);
    return ok;
}

JCE_API void JCE_CALL jce_scene_async_cancel(JceLoadHandle h)
{
    if (!g_initialised) return;
    jce_mutex_lock(g_mu);
    AsyncSlot *s = slot_lookup_locked(h);
    if (s) {
        s->cancel_requested = true;
        if (s->task) (void)jce_async_task_cancel(s->task);
        if (s->phase == SLOT_PENDING) {
            /* Never started — terminate immediately. */
            s->phase         = SLOT_CANCELLED_INT;
            s->public_status = JCE_LOAD_CANCELLED;
            s->progress      = 1.0f;
            s->reap_pending  = 1;
        }
    }
    jce_mutex_unlock(g_mu);
}

JCE_API uint32_t JCE_CALL jce_scene_async_in_flight_count(void)
{
    if (!g_initialised) return 0;
    jce_mutex_lock(g_mu);
    uint32_t n = 0;
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (!s->in_use) continue;
        if (s->public_status == JCE_LOAD_PENDING
         || s->public_status == JCE_LOAD_RUNNING) n++;
    }
    jce_mutex_unlock(g_mu);
    return n;
}
