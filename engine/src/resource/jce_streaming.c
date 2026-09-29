/*
 * jce_streaming.c  Resource streaming implementation.
 *
 * Manages chunk loading/unloading based on camera proximity.
 *
 * Threading model:
 *   - single_thread == true  (WASM default):
 *       A cooperative structured executor starts at most one deferred
 *       chunk read per update.
 *
 *   - single_thread == false (desktop/mobile default):
 *       Loads are queued on a bounded private structured executor.
 *       Main thread polls terminal task state and finalizes callbacks.
 */

#include <jce/resource/jce_streaming.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_timer.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "streaming"

#define MAX_CHUNKS          1024
#define DEFAULT_BUDGET_MS   2.0f
#define DEFAULT_MAX_PENDING 4

/* ── Internal chunk record ─────────────────────────────────────────── */

typedef struct {
    bool          registered;
    uint32_t      chunk_id;
    jce_vec3      center;
    float         radius;
    char          asset_path[256];
    JceChunkState state;
    uint64_t      estimated_size;   /* bytes (0 = unknown) */

    /* Loaded data (owned by streaming system). */
    void         *data;
    size_t        data_size;

    /* Async load tracking. */
    JceAsyncTask *pending_task;
    void         *loaded_data;
    size_t        loaded_size;
    bool          load_success;

    /* LRU bookkeeping: incremented on every frame this chunk is in
       load_radius (i.e. "actively used"). Older = better eviction candidate. */
    uint64_t      last_touch_tick;
} ChunkRecord;

/* ── Async load job context ────────────────────────────────────────── */

typedef struct {
    JceFileSystem *fs;
    char           path[256];
    ChunkRecord   *record;
} ChunkLoadJob;

/* ── System struct ─────────────────────────────────────────────────── */

struct JceStreamingSystem {
    JceStreamingConfig config;
    ChunkRecord        chunks[MAX_CHUNKS];
    uint32_t           chunk_count;
    /* Slot indices of currently-registered chunks. Kept in sync with the
       chunks[].registered flags at (un)register so the per-frame update loop
       iterates only live chunks, not the whole high-water-mark array (which
       accumulates dead slots as chunks stream out over a session). */
    uint32_t           active_idx[MAX_CHUNKS];
    uint32_t           active_count;
    uint64_t           memory_used;

    /* LRU / budget bookkeeping. */
    uint64_t           tick_counter;   /* monotonically increases each update */
    uint32_t           evicted_count;  /* total LRU evictions across lifetime */

    /* Heading-prefetch tracking (M6a).  prev_center is the camera position
       at the previous update; the per-call delta yields a heading used to
       shift the LOAD probe point ahead by config.prefetch_lead. */
    jce_vec3           prev_center;
    bool               prev_valid;

    /* Cached JCE_DISABLE_PREFETCH env toggle (-1 = unread, mirrors the
       JCE_DISABLE_WCACHE / JCE_STREAM_SYNC / JCE_ENABLE_OCCLUSION A/B
       hatches).  When set, both the heading offset and nearest-first
       ordering are skipped → exact reactive-isotropic behaviour. */
    int                prefetch_disabled;

    /* Preview / authoring load-override (default RADIUS = zero-init = exact
       current behaviour).  In ALL every registered chunk is wanted; in
       FILTER only ids in preview_filter[] are wanted.  Set via
       jce_streaming_set_preview(). */
    JceStreamPreviewMode preview_mode;
    uint32_t             preview_filter[MAX_CHUNKS];
    uint32_t             preview_filter_count;

    /* Cached JCE_DBG_PREVIEW headless hook (-1 = unread).  Reads
       "all" / "filter:1,2,3" once at first update so verification runs can
       drive the preview mode without the panel.  Only applied while the
       caller has not itself set a non-RADIUS mode (the env is a default). */
    int                  dbg_preview_read;

    /* External dependencies. */
    JceFileSystem     *fs;
    JceAsyncExecutor *executor;

    /* Callbacks. */
    JceChunkLoadedFn   on_loaded;
    JceChunkUnloadedFn on_unloaded;
    void              *callback_data;

    /* Pressure / back-pressure (P3-28). */
    JceStreamingPressure   pressure;
    JceStreamingPressure   pressure_high_water;
    uint32_t               refused_loads;
    JceStreamingPressureFn pressure_cb;
    void                  *pressure_cb_user;

    /* Engine-internal pressure hook chain (P3-A.2).  Fires before the
     * user-facing pressure_cb so layers like texture mip streaming can
     * react without consuming the single user callback slot. */
    JceStreamingPressureFn pressure_hooks[JCE_STREAMING_MAX_PRESSURE_HOOKS];
    void                  *pressure_hook_users[JCE_STREAMING_MAX_PRESSURE_HOOKS];
    uint32_t               pressure_hook_count;
};

/* Soft threshold: we enter SOFT pressure at 85% of budget. */
#define JCE_STREAMING_SOFT_THRESHOLD 0.85f

/* Live-system registration for the P3-B.3 lifecycle bridge.  Defined
 * below; forward-declared here so create/destroy can plug in. */
static void live_register(JceStreamingSystem *sys);
static void live_unregister(JceStreamingSystem *sys);

static JceStreamingPressure compute_pressure(uint64_t used, uint64_t budget)
{
    if (budget == 0) return JCE_STREAM_PRESSURE_OK;
    if (used >= budget)                                       return JCE_STREAM_PRESSURE_HARD;
    if ((double)used >= (double)budget * JCE_STREAMING_SOFT_THRESHOLD)
        return JCE_STREAM_PRESSURE_SOFT;
    return JCE_STREAM_PRESSURE_OK;
}

static void update_pressure(JceStreamingSystem *sys, uint64_t budget_bytes)
{
    JceStreamingPressure now = compute_pressure(sys->memory_used, budget_bytes);
    if (now > sys->pressure_high_water) sys->pressure_high_water = now;
    if (now != sys->pressure) {
        JceStreamingPressure prev = sys->pressure;
        sys->pressure = now;
        /* Engine-internal hooks first (e.g. texture mip streaming). */
        for (uint32_t i = 0; i < sys->pressure_hook_count; i++) {
            if (sys->pressure_hooks[i])
                sys->pressure_hooks[i](prev, now, sys->memory_used, budget_bytes,
                                       sys->pressure_hook_users[i]);
        }
        if (sys->pressure_cb)
            sys->pressure_cb(prev, now, sys->memory_used, budget_bytes,
                             sys->pressure_cb_user);
    }
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static float dist_sq(jce_vec3 a, jce_vec3 b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

static ChunkRecord *find_chunk(JceStreamingSystem *sys, uint32_t chunk_id)
{
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (sys->chunks[i].registered &&
            sys->chunks[i].chunk_id == chunk_id)
            return &sys->chunks[i];
    }
    return NULL;
}

static double now_ms(void)
{
    return (double)jce_time_perf_counter() /
           (double)jce_time_perf_freq() * 1000.0;
}

/* ── Async load worker function ───────────────────────────────────── */

static void chunk_load_job_cleanup(void *arg)
{
    JCE_FREE(arg);
}

static JceAsyncRunResult chunk_load_worker(JceAsyncContext *ctx, void *arg)
{
    ChunkLoadJob *job = (ChunkLoadJob *)arg;
    if (!job || !job->fs || !job->record) {
        jce_async_context_fail(ctx, 1, "invalid chunk load job");
        return JCE_ASYNC_RUN_FAILED;
    }
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;

    uint64_t size = 0;
    void *data = jce_fs_read_all(job->fs, job->path, &size);
    if (jce_async_context_cancel_requested(ctx)) {
        JCE_FREE(data);
        return JCE_ASYNC_RUN_CANCELLED;
    }
    if (size > (uint64_t)SIZE_MAX) {
        JCE_FREE(data);
        jce_async_context_fail(ctx, 3, "chunk exceeds address space");
        return JCE_ASYNC_RUN_FAILED;
    }

    job->record->loaded_data = data;
    job->record->loaded_size = (size_t)size;
    job->record->load_success = (data != NULL && size > 0);
    if (!job->record->load_success) {
        jce_async_context_fail(ctx, 2, "chunk read failed");
        return JCE_ASYNC_RUN_FAILED;
    }
    return JCE_ASYNC_RUN_SUCCESS;
}

/* ── Structured async load ───────────────────────────────────────── */

static bool start_chunk_load_async(JceStreamingSystem *sys, ChunkRecord *c)
{
    JceAsyncTaskDesc desc;
    if (!sys->fs || !sys->executor)
        return false;

    ChunkLoadJob *job = (ChunkLoadJob *)JCE_MALLOC(sizeof(ChunkLoadJob));
    if (!job) {
        c->state = JCE_CHUNK_UNLOADED;
        return false;
    }

    job->fs = sys->fs;
    job->record = c;
    snprintf(job->path, sizeof(job->path), "%s", c->asset_path);

    c->loaded_data = NULL;
    c->loaded_size = 0;
    c->load_success = false;

    jce_async_task_desc_init(&desc);
    desc.work = chunk_load_worker;
    desc.cleanup = chunk_load_job_cleanup;
    desc.user_data = job;
    desc.debug_name = c->asset_path;
    desc.priority = JCE_ASYNC_PRIORITY_LOW;
    c->pending_task = jce_async_submit(sys->executor, &desc);
    if (!c->pending_task) {
        JCE_FREE(job);
        c->state = JCE_CHUNK_UNLOADED;
        return false;
    }
    return true;
}

/* ── Finalize async load (main thread) ────────────────────────────── */

static void finalize_chunk_load(JceStreamingSystem *sys, ChunkRecord *c)
{
    JceAsyncState task_state;

    if (!c->pending_task ||
        !jce_async_task_is_terminal(c->pending_task))
        return;

    task_state = jce_async_task_state(c->pending_task);
    jce_async_task_release(c->pending_task);
    c->pending_task = NULL;

    if (task_state == JCE_ASYNC_STATE_SUCCEEDED && c->load_success) {
        c->data = c->loaded_data;
        c->data_size = c->loaded_size;
        c->estimated_size = c->loaded_size;
        c->state = JCE_CHUNK_LOADED;

        if (sys->on_loaded)
            sys->on_loaded(c->chunk_id, c->data, c->data_size, sys->callback_data);

        LOG_DEBUG(LOG_TAG, "async loaded chunk %u (%zu bytes)",
                  c->chunk_id, c->data_size);
    } else if (task_state == JCE_ASYNC_STATE_FAILED) {
        c->state = JCE_CHUNK_UNLOADED;
        LOG_ERROR(LOG_TAG, "async load failed for chunk %u: %s",
                  c->chunk_id, c->asset_path);
    } else {
        c->state = JCE_CHUNK_UNLOADED;
    }

    if (c->state != JCE_CHUNK_LOADED)
        JCE_FREE(c->loaded_data);
    c->loaded_data = NULL;
    c->loaded_size = 0;
}

/* ── Unload chunk ─────────────────────────────────────────────────── */

static void unload_chunk(JceStreamingSystem *sys, ChunkRecord *c)
{
    if (sys->on_unloaded)
        sys->on_unloaded(c->chunk_id, sys->callback_data);

    JCE_FREE(c->data);
    c->data = NULL;
    c->data_size = 0;
    c->state = JCE_CHUNK_UNLOADED;
    c->estimated_size = 0;
}

/* ── LRU eviction ─────────────────────────────────────────────────── *
 *
 * Evict loaded chunks that are *outside* the load radius until either:
 *   - memory_used <= budget_bytes, or
 *   - no eligible candidate remains.
 *
 * "Eligible" = LOADED state + distance to camera > load_radius.  Chunks
 * still inside load_radius are considered "actively in use" and are
 * never evicted by LRU pressure (avoids visible pop-out under camera).
 *
 * Selection: smallest last_touch_tick (least-recently-used).
 */
static uint32_t evict_lru_for_budget(JceStreamingSystem *sys,
                                     jce_vec3            camera_pos,
                                     uint64_t            budget_bytes,
                                     float               load_r2)
{
    if (budget_bytes == 0) return 0;

    uint32_t evicted = 0;
    while (sys->memory_used > budget_bytes) {
        ChunkRecord *victim = NULL;
        uint64_t     oldest = UINT64_MAX;

        for (uint32_t i = 0; i < sys->chunk_count; i++) {
            ChunkRecord *c = &sys->chunks[i];
            if (!c->registered) continue;
            if (c->state != JCE_CHUNK_LOADED) continue;
            if (dist_sq(camera_pos, c->center) <= load_r2) continue;
            if (c->last_touch_tick < oldest) {
                oldest = c->last_touch_tick;
                victim = c;
            }
        }
        if (!victim) break;  /* nothing left to evict */

        sys->memory_used -= victim->estimated_size;
        unload_chunk(sys, victim);
        sys->evicted_count++;
        evicted++;
        LOG_DEBUG(LOG_TAG, "LRU evicted chunk %u (tick=%llu)",
                  victim->chunk_id, (unsigned long long)oldest);
    }
    return evicted;
}

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceStreamingSystem *jce_streaming_create(const JceStreamingConfig *config)
{
    JceAsyncExecutorConfig async_config;
    if (!config) return NULL;

    JceStreamingSystem *sys = (JceStreamingSystem *)JCE_CALLOC(1, sizeof(*sys));
    if (!sys) return NULL;

    sys->config = *config;

    /* Apply defaults. */
    if (sys->config.max_pending == 0)
        sys->config.max_pending = DEFAULT_MAX_PENDING;
    if (sys->config.max_pending > MAX_CHUNKS)
        sys->config.max_pending = MAX_CHUNKS;
    if (sys->config.frame_budget_ms <= 0.0f)
        sys->config.frame_budget_ms = DEFAULT_BUDGET_MS;

    /* Heading-prefetch default (M6a): probe half a load-radius ahead.  This
       keeps the prefetch within the [load_radius, unload_radius] hysteresis
       band for the typical street_demo config (load 400 / unload 560 →
       lead 200), so a cell that prefetches in while approaching does not
       immediately fall back outside unload_radius on the next tick. */
    if (sys->config.prefetch_lead <= 0.0f)
        sys->config.prefetch_lead = 0.5f * sys->config.load_radius;

    sys->prefetch_disabled = -1;   /* read lazily on first update */
    sys->dbg_preview_read  = -1;   /* JCE_DBG_PREVIEW read lazily on first update */

    /* Auto-detect single-thread mode on WebAssembly. */
#if JCE_PLATFORM_WEB
    sys->config.single_thread = true;
#endif

    jce_async_executor_config_init(&async_config);
    async_config.mode = sys->config.single_thread
        ? JCE_ASYNC_EXECUTION_COOPERATIVE
        : JCE_ASYNC_EXECUTION_THREADED;
    async_config.worker_count =
        sys->config.max_pending < 4u ? sys->config.max_pending : 4u;
    async_config.reserve_latency_worker = false;
    async_config.max_tasks = sys->config.max_pending;
    async_config.cooperative_tasks_per_pump = 1;
    async_config.cooperative_completions_per_pump =
        sys->config.max_pending;
    async_config.cooperative_time_budget_us =
        (uint64_t)(sys->config.frame_budget_ms * 1000.0f);
    if (async_config.cooperative_time_budget_us == 0)
        async_config.cooperative_time_budget_us = 1;
    async_config.debug_name = "world-streaming";
    sys->executor = jce_async_executor_create(&async_config);
    if (!sys->executor) {
        JCE_FREE(sys);
        return NULL;
    }

    LOG_SUCCESS(LOG_TAG, "streaming system created (mode=%s, budget=%.1fms, %s)",
                config->mode == JCE_STREAM_RADIAL ? "radial" : "rectangular",
                (double)sys->config.frame_budget_ms,
                sys->config.single_thread ? "single-thread" : "multi-thread");
    live_register(sys);
    return sys;
}

void jce_streaming_destroy(JceStreamingSystem *sys)
{
    if (!sys) return;

    live_unregister(sys);

    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        ChunkRecord *c = &sys->chunks[i];
        if (c->pending_task)
            (void)jce_async_task_cancel(c->pending_task);
    }
    if (sys->executor) {
        (void)jce_async_executor_shutdown(
            sys->executor, JCE_ASYNC_SHUTDOWN_CANCEL_ALL,
            JCE_ASYNC_WAIT_INFINITE);
    }

    /* Release terminal handles and unload all chunks. */
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        ChunkRecord *c = &sys->chunks[i];
        if (!c->registered) continue;

        if (c->pending_task) {
            jce_async_task_release(c->pending_task);
            c->pending_task = NULL;
        }

        if (c->state == JCE_CHUNK_LOADED || c->data) {
            sys->memory_used -= c->estimated_size;
            unload_chunk(sys, c);
        }

        JCE_FREE(c->loaded_data);
    }

    if (sys->executor)
        jce_async_executor_destroy(sys->executor);
    JCE_FREE(sys);
}

/* ── Binding ──────────────────────────────────────────────────────── */

void jce_streaming_set_filesystem(JceStreamingSystem *sys, JceFileSystem *fs)
{
    if (sys) sys->fs = fs;
}

void jce_streaming_set_thread_pool(JceStreamingSystem *sys, JceThreadPool *pool)
{
    (void)sys;
    (void)pool;
}

void jce_streaming_set_callbacks(JceStreamingSystem *sys,
                                  JceChunkLoadedFn on_loaded,
                                  JceChunkUnloadedFn on_unloaded,
                                  void *user_data)
{
    if (!sys) return;
    sys->on_loaded = on_loaded;
    sys->on_unloaded = on_unloaded;
    sys->callback_data = user_data;
}

/* ── Chunk registration ───────────────────────────────────────────── */

void jce_streaming_register_chunk(JceStreamingSystem *sys,
                                   const JceStreamChunk *chunk)
{
    if (!sys || !chunk) return;

    /* Check for duplicates. */
    if (find_chunk(sys, chunk->chunk_id)) {
        LOG_WARN(LOG_TAG, "chunk %u already registered", chunk->chunk_id);
        return;
    }

    /* Reuse a slot freed by a previous unregister before growing — otherwise
     * chunk_count only ever increases and a long session that streams chunks
     * in and out exhausts MAX_CHUNKS on CUMULATIVE (not concurrent)
     * registrations (audit F68). */
    ChunkRecord *c = NULL;
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (!sys->chunks[i].registered) { c = &sys->chunks[i]; break; }
    }
    if (!c) {
        if (sys->chunk_count >= MAX_CHUNKS) {
            LOG_ERROR(LOG_TAG, "chunk limit reached (%u concurrent)", MAX_CHUNKS);
            return;
        }
        c = &sys->chunks[sys->chunk_count++];
    }
    memset(c, 0, sizeof(*c));
    c->registered = true;
    sys->active_idx[sys->active_count++] = (uint32_t)(c - sys->chunks);
    c->chunk_id   = chunk->chunk_id;
    c->center     = chunk->center;
    c->radius     = chunk->radius;
    c->state      = JCE_CHUNK_UNLOADED;

    if (chunk->asset_path) {
        size_t len = strlen(chunk->asset_path);
        if (len >= sizeof(c->asset_path))
            len = sizeof(c->asset_path) - 1;
        memcpy(c->asset_path, chunk->asset_path, len);
        c->asset_path[len] = '\0';
    }

    LOG_DEBUG(LOG_TAG, "registered chunk %u at (%.1f,%.1f,%.1f)",
              chunk->chunk_id,
              (double)chunk->center.x,
              (double)chunk->center.y,
              (double)chunk->center.z);
}

void jce_streaming_unregister_chunk(JceStreamingSystem *sys,
                                     uint32_t chunk_id)
{
    if (!sys) return;

    ChunkRecord *c = find_chunk(sys, chunk_id);
    if (!c) return;

    /* Cancel pending load. */
    if (c->pending_task) {
        (void)jce_async_task_discard(c->pending_task);
        c->pending_task = NULL;
    }

    if (c->state == JCE_CHUNK_LOADED || c->data) {
        sys->memory_used -= c->estimated_size;
        unload_chunk(sys, c);
    }

    JCE_FREE(c->loaded_data);
    c->loaded_data = NULL;
    c->registered = false;

    /* Remove this slot from the active list (swap-remove; order is irrelevant). */
    uint32_t slot = (uint32_t)(c - sys->chunks);
    for (uint32_t k = 0; k < sys->active_count; k++) {
        if (sys->active_idx[k] == slot) {
            sys->active_idx[k] = sys->active_idx[--sys->active_count];
            break;
        }
    }
}

void jce_streaming_set_chunk_residency(JceStreamingSystem *sys,
                                       uint32_t chunk_id, uint64_t bytes)
{
    if (!sys) return;
    ChunkRecord *c = find_chunk(sys, chunk_id);
    if (!c || c->state != JCE_CHUNK_LOADED) return;

    /* Swap the raw-payload estimate for the consumer's real residency so the
     * budget reflects what is actually resident, not the tiny source bytes
     * (audit F3).  LRU + pressure re-evaluate on the next update tick. */
    if (bytes >= c->estimated_size)
        sys->memory_used += (bytes - c->estimated_size);
    else
        sys->memory_used -= (c->estimated_size - bytes);
    c->estimated_size = bytes;
}

/* ── Preview / authoring load-override ────────────────────────────── */

void jce_streaming_set_preview(JceStreamingSystem *sys,
                               JceStreamPreviewMode mode,
                               const uint32_t *filter_chunk_ids,
                               uint32_t filter_count)
{
    if (!sys) return;

    sys->preview_mode = mode;
    sys->preview_filter_count = 0;

    if (mode == JCE_STREAM_PREVIEW_FILTER && filter_chunk_ids && filter_count) {
        uint32_t n = filter_count;
        if (n > MAX_CHUNKS) n = MAX_CHUNKS;
        memcpy(sys->preview_filter, filter_chunk_ids, (size_t)n * sizeof(uint32_t));
        sys->preview_filter_count = n;
    }

    LOG_INFO(LOG_TAG, "preview mode = %s (filter=%u)",
             mode == JCE_STREAM_PREVIEW_ALL    ? "ALL"    :
             mode == JCE_STREAM_PREVIEW_FILTER ? "FILTER" : "RADIUS",
             sys->preview_filter_count);
}

/* Is chunk_id in the preview filter set?  Linear scan — the set is small
   (one entry per chunk the user ticked) and only consulted in FILTER mode. */
static bool preview_filter_has(const JceStreamingSystem *sys, uint32_t chunk_id)
{
    for (uint32_t i = 0; i < sys->preview_filter_count; i++)
        if (sys->preview_filter[i] == chunk_id) return true;
    return false;
}

/* "Wanted" decision for a chunk under the active preview mode.  RADIUS uses
   the (possibly heading-shifted) load probe distance; ALL wants everything;
   FILTER wants only ids in the set. */
static bool chunk_is_wanted(const JceStreamingSystem *sys,
                            const ChunkRecord *c,
                            jce_vec3 load_center, float load_r2)
{
    switch (sys->preview_mode) {
    case JCE_STREAM_PREVIEW_ALL:
        return true;
    case JCE_STREAM_PREVIEW_FILTER:
        return preview_filter_has(sys, c->chunk_id);
    case JCE_STREAM_PREVIEW_RADIUS:
    default:
        return dist_sq(load_center, c->center) <= load_r2;
    }
}

/* Optional JCE_DBG_PREVIEW headless hook: "all" or "filter:1,2,3".  Applied
   once, only when the caller hasn't already set a non-RADIUS mode — so the
   env is a default for verification, never an override of explicit UI calls. */
static void apply_dbg_preview_env(JceStreamingSystem *sys)
{
    if (sys->dbg_preview_read >= 0) return;
    sys->dbg_preview_read = 1;

    const char *v = getenv("JCE_DBG_PREVIEW");
    if (!v || !v[0]) return;
    if (sys->preview_mode != JCE_STREAM_PREVIEW_RADIUS) return; /* UI wins */

    if (strncmp(v, "all", 3) == 0) {
        jce_streaming_set_preview(sys, JCE_STREAM_PREVIEW_ALL, NULL, 0);
        LOG_INFO(LOG_TAG, "JCE_DBG_PREVIEW=all -> preview ALL");
    } else if (strncmp(v, "filter:", 7) == 0) {
        uint32_t ids[MAX_CHUNKS];
        uint32_t n = 0;
        const char *p = v + 7;
        while (*p && n < MAX_CHUNKS) {
            char *end = NULL;
            unsigned long id = strtoul(p, &end, 10);
            if (end == p) break;            /* no digits consumed */
            ids[n++] = (uint32_t)id;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        jce_streaming_set_preview(sys, JCE_STREAM_PREVIEW_FILTER, ids, n);
        LOG_INFO(LOG_TAG, "JCE_DBG_PREVIEW=filter -> %u chunk(s)", n);
    }
}

/* ── Per-frame update ─────────────────────────────────────────────── */

void jce_streaming_update(JceStreamingSystem *sys, jce_vec3 camera_pos)
{
    JceAsyncPumpBudget async_budget;
    JCE_PROFILE_ZONE_N("Streaming::Update");
    if (!sys) { JCE_PROFILE_ZONE_END; return; }

    apply_dbg_preview_env(sys);   /* one-shot JCE_DBG_PREVIEW headless hook */

    double start = now_ms();
    jce_async_pump_budget_init(&async_budget);
    async_budget.max_work_items = 1;
    async_budget.max_completions = sys->config.max_pending;
    async_budget.max_time_us =
        (uint64_t)(sys->config.frame_budget_ms * 1000.0f);
    jce_async_executor_pump(sys->executor, &async_budget);
    float load_r2   = sys->config.load_radius   * sys->config.load_radius;
    float unload_r2 = sys->config.unload_radius * sys->config.unload_radius;
    uint64_t budget_bytes = (uint64_t)sys->config.budget_mb * 1024ULL * 1024ULL;

    /* In ALL/FILTER (preview/authoring) the user opted in to the full
       residency, so the memory-budget LRU + HARD-pressure refusal must not
       evict / refuse a WANTED chunk.  We bypass both by treating the budget
       as unlimited for the load pass while not in RADIUS.  Pressure is still
       computed at end-of-tick so the panel can warn, but it no longer gates
       loads.  RADIUS (runtime gameplay) keeps the exact budget behaviour. */
    const bool preview_active = (sys->preview_mode != JCE_STREAM_PREVIEW_RADIUS);
    const uint64_t load_budget_bytes = preview_active ? 0ULL : budget_bytes;

    sys->tick_counter++;

    /* ── Heading prefetch (M6a) ─────────────────────────────────────────
     * Derive a per-call velocity from the camera's motion since the last
     * update.  When moving, shift the LOAD probe point `prefetch_lead`
     * units along the heading so cells ahead enter the load set earlier;
     * the UNLOAD test + LRU touch keep using the ACTUAL camera position so
     * cells just behind are not prematurely evicted.  Stationary → zero
     * offset → identical to reactive streaming.  Reversing → heading flips
     * → prefetches the new direction.  Wholly disabled by
     * JCE_DISABLE_PREFETCH=1 (A/B + safety hatch; mirrors JCE_DISABLE_WCACHE
     * / JCE_STREAM_SYNC / JCE_ENABLE_OCCLUSION). */
    if (sys->prefetch_disabled < 0) {
        const char *dv = getenv("JCE_DISABLE_PREFETCH");
        sys->prefetch_disabled = (dv && dv[0] && dv[0] != '0') ? 1 : 0;
        if (sys->prefetch_disabled)
            LOG_INFO(LOG_TAG, "JCE_DISABLE_PREFETCH set — heading prefetch + "
                              "nearest-first ordering OFF (reactive-isotropic)");
    }
    const bool prefetch_on = (sys->prefetch_disabled == 0);

    jce_vec3 load_center = camera_pos;   /* default = no offset */
    if (prefetch_on && sys->prev_valid) {
        float dx = camera_pos.x - sys->prev_center.x;
        float dy = camera_pos.y - sys->prev_center.y;
        float dz = camera_pos.z - sys->prev_center.z;
        float len2 = dx * dx + dy * dy + dz * dz;
        /* Tiny-motion epsilon: ignore sub-millimetre jitter so a perfectly
         * stationary camera produces no offset (bit-identical to reactive). */
        if (len2 > 1.0e-6f) {
            float len = sqrtf(len2);
            float lead = sys->config.prefetch_lead;

            /* Clamp the lead to half the hysteresis band (unload_radius -
             * load_radius): a prefetched cell must never sit past where the cell
             * behind it unloads, or sustained high-velocity travel loads far
             * cells that evict before their spawn finishes (thrash).  band<=0 =>
             * no lead offset.  (large-world #7) */
            {
                float band = sys->config.unload_radius - sys->config.load_radius;
                float max_lead = band > 0.0f ? 0.5f * band : 0.0f;
                if (lead > max_lead) lead = max_lead;
            }

            /* Speed-ramp the lead by how far the camera actually moved this
             * tick, saturating at the full lead.  A bare normalize-then-scale
             * would throw the probe a FULL `lead` units in the direction of
             * even sub-unit physics drift / settle jitter (a player standing
             * still still slides ~0.1 u/tick as the body grounds), spuriously
             * prefetching cells off to the side.  Ramping over a reference
             * distance keeps micro-drift → micro-offset and only sustained
             * travel reaches the full lead — and it is frame-rate independent
             * (slower per-tick deltas at higher FPS still ramp by accumulated
             * displacement).  Reference = 1/16 of the lead: at street_demo's
             * lead=200 that is ~12.5 u/tick to saturate, well above settle
             * jitter yet reached within a couple of frames of real walking. */
            const float ramp_ref = lead * (1.0f / 16.0f);
            float gain = (ramp_ref > 0.0f) ? (len / ramp_ref) : 1.0f;
            if (gain > 1.0f) gain = 1.0f;

            float scale = (lead * gain) / len;   /* offset_len = lead*gain */
            load_center.x = camera_pos.x + dx * scale;
            load_center.y = camera_pos.y + dy * scale;
            load_center.z = camera_pos.z + dz * scale;
        }
    }
    sys->prev_center = camera_pos;
    sys->prev_valid  = true;

    uint32_t loads_this_frame   = 0;
    uint32_t pending_count      = 0;
    /* Cap async-completion finalizes per frame: finalizing many chunks at once
     * batches their entity-spawn into a hitch.  A completed-but-not-finalized
     * chunk stays LOADING and finalizes a later frame (its slot is held, which
     * naturally throttles new loads).  (large-world #7) */
    uint32_t finalized_this_frame = 0;
    const uint32_t kMaxFinalizePerFrame = 2u;

    /* ── Pass 1: order-independent housekeeping (LRU touch, async-completion
     * finalize, unload, unloading-transition) + count in-flight loads.  The
     * LOAD decision is pulled out into a separate nearest-first pre-pass
     * (pass 2) below.  Everything here is unaffected by iteration order. */
    for (uint32_t k = 0; k < sys->active_count; k++) {
        ChunkRecord *c = &sys->chunks[sys->active_idx[k]];
        if (!c->registered) continue;   /* defensive; active list should be live-only */

        float d2 = dist_sq(camera_pos, c->center);   /* ACTUAL pos for unload/LRU */

        /* Whether this chunk is wanted under the active preview mode.  RADIUS
         * uses the in-radius test (load probe); ALL wants everything; FILTER
         * wants only ids in the set.  Drives both the LRU touch (keep wanted
         * chunks resident) and the unload decision below. */
        const bool wanted = chunk_is_wanted(sys, c, camera_pos, load_r2);

        /* Touch wanted loaded chunks so LRU keeps them resident. */
        if (c->state == JCE_CHUNK_LOADED && wanted)
            c->last_touch_tick = sys->tick_counter;

        switch (c->state) {
        case JCE_CHUNK_LOADING:
            pending_count++;

            /* Check for async completion (rate-limited; see kMaxFinalizePerFrame). */
            if (c->pending_task &&
                jce_async_task_is_terminal(c->pending_task) &&
                finalized_this_frame < kMaxFinalizePerFrame) {
                finalize_chunk_load(sys, c);
                if (pending_count > 0)
                    pending_count--;
                finalized_this_frame++;
                if (c->state == JCE_CHUNK_LOADED) {
                    sys->memory_used += c->estimated_size;
                }
            }
            break;

        case JCE_CHUNK_LOADED:
            /* Should we unload this chunk?
             *   RADIUS : ACTUAL-camera distance beyond unload_radius (so we
             *            never unload cells just behind the player while
             *            prefetching ahead).
             *   ALL    : never unload by distance (whole world stays resident).
             *   FILTER : unload chunks that are loaded but no longer wanted
             *            (user unticked them).  No distance hysteresis — the
             *            user's tick is the authority. */
            {
                bool should_unload;
                if (sys->preview_mode == JCE_STREAM_PREVIEW_ALL)
                    should_unload = false;
                else if (sys->preview_mode == JCE_STREAM_PREVIEW_FILTER)
                    should_unload = !wanted;
                else
                    should_unload = (d2 > unload_r2);

                if (should_unload) {
                    c->state = JCE_CHUNK_UNLOADING;
                    sys->memory_used -= c->estimated_size;
                    unload_chunk(sys, c);
                    LOG_TRACE(LOG_TAG, "unloaded chunk %u", c->chunk_id);
                }
            }
            break;

        case JCE_CHUNK_UNLOADING:
            /* Unload is synchronous — immediately transition. */
            c->state = JCE_CHUNK_UNLOADED;
            break;

        case JCE_CHUNK_UNLOADED:
        default:
            break;   /* loads handled in pass 2 */
        }
    }

    /* ── Pass 2: nearest-first LOAD pre-pass (M6a) ──────────────────────
     * Collect UNLOADED candidates that are within load_radius of the
     * (possibly heading-shifted) load_center, sort them nearest-first by
     * distance to load_center, then consume the per-frame load slots in that
     * order so the most-urgent (closest / most-in-front) cells load first.
     * Temp arrays are bounded by active_count (already capped at MAX_CHUNKS).
     * When prefetch is OFF this still loads nearest-first relative to the
     * camera, which is order-independent vs. the old registration order for
     * the resident SET (only the within-frame slot priority differs) — so
     * the A/B difference attributable to prefetch is the heading offset. */
    uint32_t cand_idx[MAX_CHUNKS];
    float    cand_d2 [MAX_CHUNKS];
    uint32_t cand_count = 0;

    for (uint32_t k = 0; k < sys->active_count; k++) {
        uint32_t slot = sys->active_idx[k];
        ChunkRecord *c = &sys->chunks[slot];
        if (!c->registered || c->state != JCE_CHUNK_UNLOADED) continue;

        /* WANTED gate (preview-aware).  In RADIUS this is the in-radius test;
         * in ALL/FILTER it is the all/filter test.  The nearest-first sort key
         * below still uses the load-probe distance so the closest wanted cells
         * stream in first even when ALL/FILTER wants far ones too. */
        if (!chunk_is_wanted(sys, c, load_center, load_r2)) continue;

        float ld2 = dist_sq(load_center, c->center);
        cand_idx[cand_count] = slot;
        cand_d2 [cand_count] = ld2;
        cand_count++;
    }

    /* Insertion sort: candidate counts in-radius are small per frame (bounded
     * by the ring area / chunk size) and this preserves stability cheaply. */
    for (uint32_t i = 1; i < cand_count; i++) {
        float    kd = cand_d2[i];
        uint32_t ki = cand_idx[i];
        uint32_t j  = i;
        while (j > 0 && cand_d2[j - 1] > kd) {
            cand_d2[j]  = cand_d2[j - 1];
            cand_idx[j] = cand_idx[j - 1];
            j--;
        }
        cand_d2[j]  = kd;
        cand_idx[j] = ki;
    }

    for (uint32_t i = 0; i < cand_count; i++) {
        if (pending_count + loads_this_frame >= sys->config.max_pending)
            break;

        ChunkRecord *c = &sys->chunks[cand_idx[i]];

        /* Try to make room via LRU before refusing.  Eviction protects cells
         * inside load_radius of the ACTUAL camera (camera_pos), not the
         * shifted probe — so prefetched-ahead cells stay evictable but cells
         * around the player do not.  load_budget_bytes is 0 (unlimited) in
         * ALL/FILTER preview so a wanted chunk is never evicted/refused — the
         * user opted in to the heavier residency (see preview_active above). */
        if (load_budget_bytes != 0 && sys->memory_used >= load_budget_bytes)
            evict_lru_for_budget(sys, camera_pos, load_budget_bytes, load_r2);

        if (load_budget_bytes != 0 && sys->memory_used >= load_budget_bytes) {
            sys->refused_loads++;
            break;  /* still over budget — defer to next frame */
        }

        c->state = JCE_CHUNK_LOADING;

        if (!start_chunk_load_async(sys, c))
            continue;
        loads_this_frame++;

        LOG_TRACE(LOG_TAG, "started loading chunk %u", c->chunk_id);

        /* M6a A/B instrumentation: at the moment a chunk STARTS loading, log
         * the ACTUAL camera→chunk-center distance (the "lead distance").  With
         * prefetch ON cells start loading at a greater lead distance than OFF.
         * Gated behind JCE_STREAM_LOADLOG=1 so normal runs are quiet. */
        {
            static int s_loadlog = -1;
            if (s_loadlog < 0) {
                const char *lv = getenv("JCE_STREAM_LOADLOG");
                s_loadlog = (lv && lv[0] && lv[0] != '0') ? 1 : 0;
            }
            if (s_loadlog) {
                float lead = sqrtf(dist_sq(camera_pos, c->center));
                LOG_INFO(LOG_TAG,
                         "LOADSTART chunk=%u lead=%.1f cam=(%.1f,%.1f,%.1f) "
                         "ctr=(%.1f,%.1f,%.1f) prefetch=%d",
                         c->chunk_id, (double)lead,
                         (double)camera_pos.x, (double)camera_pos.y, (double)camera_pos.z,
                         (double)c->center.x, (double)c->center.y, (double)c->center.z,
                         prefetch_on ? 1 : 0);
            }
        }

        /* Cooperative mode defers actual reads to the next budgeted pump. */
        if (sys->config.single_thread && loads_this_frame > 0) {
            double elapsed = now_ms() - start;
            if (elapsed >= (double)sys->config.frame_budget_ms)
                break;  /* resume next frame */
        }
    }

    /* End-of-tick: reclassify pressure based on final memory_used.
     * Fires the optional callback only on level transitions. */
    update_pressure(sys, budget_bytes);

    JCE_PROFILE_ZONE_END;
}

/* ── Queries ──────────────────────────────────────────────────────── */

uint32_t jce_streaming_loaded_count(const JceStreamingSystem *sys)
{
    if (!sys) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (sys->chunks[i].registered &&
            sys->chunks[i].state == JCE_CHUNK_LOADED)
            count++;
    }
    return count;
}

uint32_t jce_streaming_pending_count(const JceStreamingSystem *sys)
{
    if (!sys) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (sys->chunks[i].registered &&
            sys->chunks[i].state == JCE_CHUNK_LOADING)
            count++;
    }
    return count;
}

uint64_t jce_streaming_memory_used(const JceStreamingSystem *sys)
{
    return sys ? sys->memory_used : 0;
}

uint32_t jce_streaming_evicted_count(const JceStreamingSystem *sys)
{
    return sys ? sys->evicted_count : 0;
}

JceStreamingPressure jce_streaming_get_pressure(const JceStreamingSystem *sys)
{
    return sys ? sys->pressure : JCE_STREAM_PRESSURE_OK;
}

JceStreamingPressure jce_streaming_pressure_high_water(const JceStreamingSystem *sys)
{
    return sys ? sys->pressure_high_water : JCE_STREAM_PRESSURE_OK;
}

const char *jce_streaming_pressure_name(JceStreamingPressure p)
{
    switch (p) {
    case JCE_STREAM_PRESSURE_OK:   return "OK";
    case JCE_STREAM_PRESSURE_SOFT: return "SOFT";
    case JCE_STREAM_PRESSURE_HARD: return "HARD";
    default:                       return "?";
    }
}

uint32_t jce_streaming_refused_loads(const JceStreamingSystem *sys)
{
    return sys ? sys->refused_loads : 0;
}

void jce_streaming_set_pressure_callback(JceStreamingSystem    *sys,
                                          JceStreamingPressureFn fn,
                                          void                  *user)
{
    if (!sys) return;
    sys->pressure_cb      = fn;
    sys->pressure_cb_user = user;
}

bool jce_streaming_add_pressure_hook(JceStreamingSystem    *sys,
                                      JceStreamingPressureFn fn,
                                      void                  *user)
{
    if (!sys || !fn) return false;
    if (sys->pressure_hook_count >= JCE_STREAMING_MAX_PRESSURE_HOOKS) {
        LOG_WARN(LOG_TAG, "pressure hook table full (%d) — refusing registration",
                 JCE_STREAMING_MAX_PRESSURE_HOOKS);
        return false;
    }
    uint32_t i = sys->pressure_hook_count++;
    sys->pressure_hooks[i]      = fn;
    sys->pressure_hook_users[i] = user;
    return true;
}

/* ── Low-memory broadcast (P3-B.3 lifecycle bridge) ───────────────── */

/* Track every live streaming system so the engine's lifecycle listener
 * can broadcast OS LOW_MEMORY signals without holding an explicit
 * reference.  Bounded fixed array — typical apps create 0..2 systems. */
#define JCE_STREAMING_MAX_LIVE 8
static JceStreamingSystem *s_live_systems[JCE_STREAMING_MAX_LIVE];
static uint32_t            s_live_count;

static void live_register(JceStreamingSystem *sys)
{
    if (!sys) return;
    if (s_live_count >= JCE_STREAMING_MAX_LIVE) {
        LOG_WARN(LOG_TAG, "live system table full (%d) — low-memory broadcast may miss this system",
                 JCE_STREAMING_MAX_LIVE);
        return;
    }
    s_live_systems[s_live_count++] = sys;
}

static void live_unregister(JceStreamingSystem *sys)
{
    if (!sys) return;
    for (uint32_t i = 0; i < s_live_count; i++) {
        if (s_live_systems[i] != sys) continue;
        const uint32_t tail = s_live_count - i - 1u;
        if (tail > 0u)
            memmove(&s_live_systems[i], &s_live_systems[i + 1u],
                    (size_t)tail * sizeof(s_live_systems[0]));
        s_live_count--;
        return;
    }
}

void jce_streaming_signal_low_memory(JceStreamingSystem *sys)
{
    if (!sys) return;

    JceStreamingPressure prev = sys->pressure;
    if (prev == JCE_STREAM_PRESSURE_HARD) {
        /* Re-fire hooks anyway so listeners can free additional caches
         * each time the OS nags us; treat it as a HARD→HARD pulse. */
    } else {
        sys->pressure            = JCE_STREAM_PRESSURE_HARD;
        sys->pressure_high_water = JCE_STREAM_PRESSURE_HARD;
    }

    const uint64_t budget = (uint64_t)sys->config.budget_mb * 1024ULL * 1024ULL;
    for (uint32_t i = 0; i < sys->pressure_hook_count; i++) {
        if (sys->pressure_hooks[i])
            sys->pressure_hooks[i](prev, JCE_STREAM_PRESSURE_HARD,
                                   sys->memory_used, budget,
                                   sys->pressure_hook_users[i]);
    }
    if (sys->pressure_cb)
        sys->pressure_cb(prev, JCE_STREAM_PRESSURE_HARD,
                         sys->memory_used, budget,
                         sys->pressure_cb_user);

    LOG_INFO(LOG_TAG, "low-memory signal: pressure forced to HARD (used=%llu budget=%llu)",
             (unsigned long long)sys->memory_used,
             (unsigned long long)budget);
}

void jce_streaming_signal_low_memory_all(void)
{
    for (uint32_t i = 0; i < s_live_count; i++)
        jce_streaming_signal_low_memory(s_live_systems[i]);
}

bool jce_streaming_chunk_loaded(const JceStreamingSystem *sys,
                                 uint32_t chunk_id)
{
    if (!sys) return false;
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (sys->chunks[i].registered &&
            sys->chunks[i].chunk_id == chunk_id)
            return sys->chunks[i].state == JCE_CHUNK_LOADED;
    }
    return false;
}

JceChunkState jce_streaming_chunk_state(const JceStreamingSystem *sys,
                                        uint32_t chunk_id)
{
    if (!sys) return JCE_CHUNK_UNLOADED;
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (sys->chunks[i].registered &&
            sys->chunks[i].chunk_id == chunk_id)
            return sys->chunks[i].state;
    }
    return JCE_CHUNK_UNLOADED;
}

void *jce_streaming_chunk_data(const JceStreamingSystem *sys,
                                uint32_t chunk_id, size_t *out_size)
{
    if (!sys) return NULL;
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (sys->chunks[i].registered &&
            sys->chunks[i].chunk_id == chunk_id &&
            sys->chunks[i].state == JCE_CHUNK_LOADED) {
            if (out_size) *out_size = sys->chunks[i].data_size;
            return sys->chunks[i].data;
        }
    }
    if (out_size) *out_size = 0;
    return NULL;
}

bool jce_streaming_is_single_thread(const JceStreamingSystem *sys)
{
    return sys ? sys->config.single_thread : false;
}
