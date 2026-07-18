/*
 * jce_scene_async.c  Async scene / prefab instantiate (P3-A.3).
 *
 * Design rationale (see jce_scene_async.h for the public contract):
 *
 *  - A small fixed slot table (JCE_ASYNC_SLOTS) holds all in-flight
 *    loads.  Handles encode a (generation, index) pair so stale handles
 *    are detectable after a slot is reused.
 *  - Each load owns a dedicated jce_thread that performs the I/O +
 *    JSON parse in the background.  We deliberately do NOT use
 *    jce_jobs (enkiTS) here: the engine has no global JceJobSystem
 *    instance, and a one-shot dedicated thread per load matches the
 *    "long-lived" guidance from jce_thread.h.
 *  - Apply-to-flecs happens on the main thread.  flecs is not safe for
 *    concurrent entity creation against a live world without manual
 *    deferral, and the existing jce_scene_load_json() routine is a
 *    two-pass operation (create + parent-fixup) that doesn't slice
 *    cleanly.  We therefore commit at most ONE finished load per
 *    dispatch_main call to bound per-frame spikes.
 *  - Progress is updated atomically by the worker (40 % weight on parse
 *    completion) and by the main thread (60 % weight when apply lands).
 *  - Cancellation is cooperative: an atomic flag is checked at the
 *    file-read, post-parse, and pre-apply boundaries.
 */

#include <jce/middleware/scene/jce_scene_async.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
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

    /* Worker handoff. */
    JceThread     *worker;
    SlotPhase      phase;          /* main-thread visible phase           */
    JceAtomicI32  *cancel_flag;    /* 0 = run, 1 = cancel                 */
    JceAtomicI32  *worker_done;    /* 0 = working, 1 = parsed/failed      */

    /* Parsed payload (produced by worker, consumed by main). */
    JceJson       *parsed_root;    /* NULL if parse failed                */
    uint32_t       parsed_entity_count;
    uint32_t       worker_error;   /* non-zero on parse/IO failure        */

    /* Output. */
    JceLoadResult  result;
    JceLoadStatus  public_status;  /* what callers see via _status()      */
    float          progress;       /* 0..1, monotonic per slot            */
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
    if (s->cancel_flag) {
        jce_atomic_i32_destroy(s->cancel_flag);
        s->cancel_flag = NULL;
    }
    if (s->worker_done) {
        jce_atomic_i32_destroy(s->worker_done);
        s->worker_done = NULL;
    }
}

static void slot_reap(AsyncSlot *s)
{
    /* Worker (if still alive) MUST be joined before we touch payload.
     * Reap is only called from the main thread after the worker has
     * signalled worker_done OR a cancel has propagated through.
     */
    if (s->worker) {
        jce_thread_join(s->worker);
        s->worker = NULL;
    }
    slot_free_payload(s);
    uint16_t next_gen = (uint16_t)(s->generation + 1u);
    memset(s, 0, sizeof(*s));
    s->generation = next_gen;
}

/* ── Worker entry ─────────────────────────────────────────────────── */

typedef struct {
    AsyncSlot *slot;
} WorkerArg;

static void worker_main(void *arg)
{
    WorkerArg *wa = (WorkerArg *)arg;
    AsyncSlot *s  = wa->slot;
    JCE_FREE(wa);

    /* Snapshot inputs we need (vfs_path is stable for the slot lifetime
     * — main thread only frees it during reap, which is gated on
     * worker_done). */
    const char *path = s->vfs_path;

    if (jce_atomic_i32_load(s->cancel_flag) != 0) {
        s->worker_error = 0;
        jce_atomic_i32_store(s->worker_done, 1);
        return;
    }

    uint64_t size = 0;
    void    *buf  = NULL;
    if (g_target_fs) {
        buf = jce_fs_read_all(g_target_fs, path, &size);
    } else {
        buf = jce_fs_host_read_all(path, &size);
    }

    if (!buf || size == 0) {
        if (buf) JCE_FREE(buf);
        LOG_ERROR(LOG_TAG, "cannot read '%s'", path ? path : "(null)");
        s->worker_error = 1; /* I/O */
        jce_atomic_i32_store(s->worker_done, 1);
        return;
    }

    /* P3-A.5: account the staging buffer against the SCENE_ECS tag for
     * the brief window between read and parse. Freed below regardless
     * of the parse outcome. */
    jce_mem_profile_record_alloc(JCE_MEM_TAG_SCENE_ECS, (size_t)size);

    if (jce_atomic_i32_load(s->cancel_flag) != 0) {
        JCE_FREE(buf);
        jce_mem_profile_record_free(JCE_MEM_TAG_SCENE_ECS, (size_t)size);
        jce_atomic_i32_store(s->worker_done, 1);
        return;
    }

    JceJson *root = jce_json_parse((const char *)buf, (size_t)size);
    JCE_FREE(buf);
    jce_mem_profile_record_free(JCE_MEM_TAG_SCENE_ECS, (size_t)size);

    if (!root) {
        LOG_ERROR(LOG_TAG, "JSON parse failed for '%s'", path);
        s->worker_error = 2; /* parse */
        jce_atomic_i32_store(s->worker_done, 1);
        return;
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
    jce_atomic_i32_store(s->worker_done, 1);
    /* From this point the main thread owns parsed_root. */
}

/* ── Player-loop tick ─────────────────────────────────────────────── */

static void count_cb_main(JceScene *sc, JceEntity e, void *ud)
{
    (void)sc; (void)e;
    *(uint32_t *)ud += 1u;
}

static void apply_one_locked(AsyncSlot *s)
{
    /* Pre-conditions: holding g_mu, phase == SLOT_PARSED, worker joined. */
    if (jce_atomic_i32_load(s->cancel_flag) != 0) {
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

    int n = g_target_fs
        ? jce_scene_serial_apply_json_vfs(g_target_scene, g_target_fs,
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
        if (s->in_use && s->reap_pending) {
            /* Move public status into a non-readable state by reaping;
             * callers that wanted the result should have grabbed it via
             * get_result() before the next tick. */
            slot_reap(s);
        }
    }

    /* Stage 2: promote PENDING → PARSING (spawn workers). */
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
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
        wa->slot   = s;
        s->phase   = SLOT_PARSING;
        s->public_status = JCE_LOAD_RUNNING;
        s->worker  = jce_thread_create(worker_main, wa, "jce-scene-async");
        if (!s->worker) {
            JCE_FREE(wa);
            s->phase             = SLOT_FAILED_INT;
            s->public_status     = JCE_LOAD_FAILED;
            s->result.error_code = 6; /* thread spawn */
            s->progress          = 1.0f;
            s->reap_pending      = 1;
        }
    }

    /* Stage 3: harvest worker_done → PARSED / FAILED / CANCELLED. */
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (!s->in_use || s->phase != SLOT_PARSING) continue;
        if (jce_atomic_i32_load(s->worker_done) == 0) continue;

        /* Worker finished — join thread so its stack/handle is reclaimed. */
        if (s->worker) {
            jce_thread_join(s->worker);
            s->worker = NULL;
        }

        if (jce_atomic_i32_load(s->cancel_flag) != 0) {
            s->phase         = SLOT_CANCELLED_INT;
            s->public_status = JCE_LOAD_CANCELLED;
            s->progress      = 1.0f;
            s->reap_pending  = 1;
            continue;
        }
        if (s->worker_error != 0 || !s->parsed_root) {
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
        if (!s->cb) continue;
        if (s->public_status != JCE_LOAD_COMPLETE
         && s->public_status != JCE_LOAD_FAILED
         && s->public_status != JCE_LOAD_CANCELLED) continue;

        pending[npending].h      = handle_make((uint16_t)i, s->generation);
        pending[npending].status = s->public_status;
        pending[npending].result = s->result;
        pending[npending].cb     = s->cb;
        pending[npending].user   = s->user;
        npending++;
        /* Clear the cb so the callback never fires twice across ticks. */
        s->cb = NULL;
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

    /* Signal cancel on every live slot, then drain. */
    jce_mutex_lock(g_mu);
    for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
        AsyncSlot *s = &g_slots[i];
        if (!s->in_use) continue;
        if (s->cancel_flag) jce_atomic_i32_store(s->cancel_flag, 1);
    }
    jce_mutex_unlock(g_mu);

    /* Wait for all workers to observe the cancel. */
    for (;;) {
        bool any_active = false;
        jce_mutex_lock(g_mu);
        for (uint32_t i = 0; i < JCE_ASYNC_SLOTS; i++) {
            AsyncSlot *s = &g_slots[i];
            if (!s->in_use) continue;
            if (s->phase == SLOT_PARSING
             && jce_atomic_i32_load(s->worker_done) == 0) {
                any_active = true;
            }
        }
        jce_mutex_unlock(g_mu);
        if (!any_active) break;
        jce_thread_sleep_ms(1);
    }

    jce_mutex_lock(g_mu);
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
    s->phase         = SLOT_PENDING;
    s->public_status = JCE_LOAD_PENDING;
    s->progress      = 0.0f;

    size_t pl = strlen(vfs_path);
    s->vfs_path = (char *)JCE_MALLOC(pl + 1u);
    if (s->vfs_path) memcpy(s->vfs_path, vfs_path, pl + 1u);

    s->cancel_flag = jce_atomic_i32_create(0);
    s->worker_done = jce_atomic_i32_create(0);

    if (!s->vfs_path || !s->cancel_flag || !s->worker_done) {
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

JCE_API float JCE_CALL jce_scene_async_progress(JceLoadHandle h)
{
    if (!g_initialised) return 0.0f;
    jce_mutex_lock(g_mu);
    AsyncSlot *s = slot_lookup_locked(h);
    float p = 0.0f;
    if (s) {
        /* Smoothly advance progress while the worker runs so callers
         * see motion before the parse boundary lands. */
        if (s->phase == SLOT_PARSING && s->progress < 0.35f) {
            s->progress += 0.01f;
            if (s->progress > 0.35f) s->progress = 0.35f;
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
    if (s && s->cancel_flag) {
        jce_atomic_i32_store(s->cancel_flag, 1);
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
