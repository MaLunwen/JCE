/*
 * jce_streaming.c  Resource streaming implementation.
 *
 * Manages chunk loading/unloading based on camera proximity.
 *
 * Threading model:
 *   - single_thread == true  (WASM default):
 *       Each call to jce_streaming_update() processes at most one
 *       pending load/unload within the frame time budget.  This is a
 *       cooperative "coroutine-style" approach that avoids blocking
 *       the main thread.
 *
 *   - single_thread == false (desktop/mobile default):
 *       Loads are queued and dispatched to background threads via
 *       JceThreadPool.  Main thread polls for completion.
 */

#include <jce/streaming/jce_streaming.h>
#include <jce/core/jce_filesystem.h>
#include <jce/core/jce_thread.h>
#include <jce/core/jce_log.h>
#include <jce/core/jce_profiler.h>
#include "core/jce_memory.h"

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_atomic.h>

#include <string.h>
#include <stdlib.h>
#include <math.h>

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
    JceTask      *pending_task;
    SDL_AtomicInt load_complete;
    void         *loaded_data;
    size_t        loaded_size;
    bool          load_success;
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
    uint64_t           memory_used;

    /* External dependencies. */
    JceFileSystem     *fs;
    JceThreadPool     *thread_pool;

    /* Callbacks. */
    JceChunkLoadedFn   on_loaded;
    JceChunkUnloadedFn on_unloaded;
    void              *callback_data;
};

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
    return (double)SDL_GetPerformanceCounter() /
           (double)SDL_GetPerformanceFrequency() * 1000.0;
}

/* ── Async load worker function ───────────────────────────────────── */

static void chunk_load_worker(void *arg)
{
    ChunkLoadJob *job = (ChunkLoadJob *)arg;
    if (!job || !job->fs || !job->record) {
        if (job) JCE_FREE(job);
        return;
    }

    size_t size = 0;
    void *data = jce_fs_read_all(job->fs, job->path, &size);

    job->record->loaded_data = data;
    job->record->loaded_size = size;
    job->record->load_success = (data != NULL && size > 0);

    /* Signal completion (memory barrier). */
    SDL_SetAtomicInt(&job->record->load_complete, 1);

    JCE_FREE(job);
}

/* ── Synchronous load (single-thread mode) ────────────────────────── */

static bool load_chunk_sync(JceStreamingSystem *sys, ChunkRecord *c)
{
    if (!sys->fs) {
        LOG_ERROR(LOG_TAG, "no filesystem bound");
        return false;
    }

    size_t size = 0;
    void *data = jce_fs_read_all(sys->fs, c->asset_path, &size);

    if (!data || size == 0) {
        LOG_ERROR(LOG_TAG, "failed to load chunk %u: %s",
                  c->chunk_id, c->asset_path);
        return false;
    }

    c->data = data;
    c->data_size = size;
    c->estimated_size = size;
    c->state = JCE_CHUNK_LOADED;

    if (sys->on_loaded)
        sys->on_loaded(c->chunk_id, c->data, c->data_size, sys->callback_data);

    LOG_DEBUG(LOG_TAG, "loaded chunk %u (%zu bytes): %s",
              c->chunk_id, size, c->asset_path);
    return true;
}

/* ── Async load (multi-thread mode) ──────────────────────────────── */

static void start_chunk_load_async(JceStreamingSystem *sys, ChunkRecord *c)
{
    if (!sys->fs || !sys->thread_pool) {
        /* Fall back to sync if no pool available. */
        load_chunk_sync(sys, c);
        return;
    }

    ChunkLoadJob *job = (ChunkLoadJob *)JCE_MALLOC(sizeof(ChunkLoadJob));
    if (!job) {
        c->state = JCE_CHUNK_UNLOADED;
        return;
    }

    job->fs = sys->fs;
    job->record = c;
    snprintf(job->path, sizeof(job->path), "%s", c->asset_path);

    SDL_SetAtomicInt(&c->load_complete, 0);
    c->loaded_data = NULL;
    c->loaded_size = 0;
    c->load_success = false;

    c->pending_task = jce_thread_pool_submit_tracked(sys->thread_pool,
                                                      chunk_load_worker, job);
}

/* ── Finalize async load (main thread) ────────────────────────────── */

static void finalize_chunk_load(JceStreamingSystem *sys, ChunkRecord *c)
{
    if (!SDL_GetAtomicInt(&c->load_complete))
        return;

    if (c->pending_task) {
        jce_task_wait(c->pending_task);
        jce_task_free(c->pending_task);
        c->pending_task = NULL;
    }

    if (c->load_success) {
        c->data = c->loaded_data;
        c->data_size = c->loaded_size;
        c->estimated_size = c->loaded_size;
        c->state = JCE_CHUNK_LOADED;

        if (sys->on_loaded)
            sys->on_loaded(c->chunk_id, c->data, c->data_size, sys->callback_data);

        LOG_DEBUG(LOG_TAG, "async loaded chunk %u (%zu bytes)",
                  c->chunk_id, c->data_size);
    } else {
        c->state = JCE_CHUNK_UNLOADED;
        LOG_ERROR(LOG_TAG, "async load failed for chunk %u: %s",
                  c->chunk_id, c->asset_path);
    }

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

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceStreamingSystem *jce_streaming_create(const JceStreamingConfig *config)
{
    if (!config) return NULL;

    JceStreamingSystem *sys = (JceStreamingSystem *)JCE_CALLOC(1, sizeof(*sys));
    if (!sys) return NULL;

    sys->config = *config;

    /* Apply defaults. */
    if (sys->config.max_pending == 0)
        sys->config.max_pending = DEFAULT_MAX_PENDING;
    if (sys->config.frame_budget_ms <= 0.0f)
        sys->config.frame_budget_ms = DEFAULT_BUDGET_MS;

    /* Auto-detect single-thread mode on Emscripten. */
#ifdef __EMSCRIPTEN__
    sys->config.single_thread = true;
#endif

    LOG_SUCCESS(LOG_TAG, "streaming system created (mode=%s, budget=%.1fms, %s)",
                config->mode == JCE_STREAM_RADIAL ? "radial" : "rectangular",
                (double)sys->config.frame_budget_ms,
                sys->config.single_thread ? "single-thread" : "multi-thread");
    return sys;
}

void jce_streaming_destroy(JceStreamingSystem *sys)
{
    if (!sys) return;

    /* Wait for pending loads and unload all chunks. */
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        ChunkRecord *c = &sys->chunks[i];
        if (!c->registered) continue;

        if (c->pending_task) {
            jce_task_wait(c->pending_task);
            jce_task_free(c->pending_task);
            c->pending_task = NULL;
        }

        if (c->state == JCE_CHUNK_LOADED || c->data) {
            sys->memory_used -= c->estimated_size;
            unload_chunk(sys, c);
        }

        JCE_FREE(c->loaded_data);
    }

    JCE_FREE(sys);
}

/* ── Binding ──────────────────────────────────────────────────────── */

void jce_streaming_set_filesystem(JceStreamingSystem *sys, JceFileSystem *fs)
{
    if (sys) sys->fs = fs;
}

void jce_streaming_set_thread_pool(JceStreamingSystem *sys, JceThreadPool *pool)
{
    if (sys) {
        sys->thread_pool = pool;
        /* If no pool, force single-thread mode. */
        if (!pool) sys->config.single_thread = true;
    }
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

    if (sys->chunk_count >= MAX_CHUNKS) {
        LOG_ERROR(LOG_TAG, "chunk limit reached (%u)", MAX_CHUNKS);
        return;
    }

    /* Check for duplicates. */
    if (find_chunk(sys, chunk->chunk_id)) {
        LOG_WARN(LOG_TAG, "chunk %u already registered", chunk->chunk_id);
        return;
    }

    ChunkRecord *c = &sys->chunks[sys->chunk_count++];
    memset(c, 0, sizeof(*c));
    c->registered = true;
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
        jce_task_wait(c->pending_task);
        jce_task_free(c->pending_task);
        c->pending_task = NULL;
    }

    if (c->state == JCE_CHUNK_LOADED || c->data) {
        sys->memory_used -= c->estimated_size;
        unload_chunk(sys, c);
    }

    JCE_FREE(c->loaded_data);
    c->loaded_data = NULL;
    c->registered = false;
}

/* ── Per-frame update ─────────────────────────────────────────────── */

void jce_streaming_update(JceStreamingSystem *sys, jce_vec3 camera_pos)
{
    JCE_PROFILE_ZONE_N("Streaming::Update");
    if (!sys) { JCE_PROFILE_ZONE_END; return; }

    double start = now_ms();
    float load_r2   = sys->config.load_radius   * sys->config.load_radius;
    float unload_r2 = sys->config.unload_radius * sys->config.unload_radius;
    uint64_t budget_bytes = (uint64_t)sys->config.budget_mb * 1024ULL * 1024ULL;

    uint32_t loads_this_frame   = 0;
    uint32_t pending_count      = 0;

    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        ChunkRecord *c = &sys->chunks[i];
        if (!c->registered) continue;

        float d2 = dist_sq(camera_pos, c->center);

        switch (c->state) {
        case JCE_CHUNK_UNLOADED:
            /* Should we load this chunk? */
            if (d2 <= load_r2 &&
                pending_count + loads_this_frame < sys->config.max_pending &&
                (budget_bytes == 0 || sys->memory_used < budget_bytes)) {

                c->state = JCE_CHUNK_LOADING;

                if (sys->config.single_thread) {
                    /* Synchronous load within frame budget. */
                    if (load_chunk_sync(sys, c)) {
                        sys->memory_used += c->estimated_size;
                    }
                    loads_this_frame++;
                } else {
                    /* Async load via thread pool. */
                    start_chunk_load_async(sys, c);
                }

                LOG_TRACE(LOG_TAG, "started loading chunk %u", c->chunk_id);
            }
            break;

        case JCE_CHUNK_LOADING:
            pending_count++;

            /* Check for async completion. */
            if (!sys->config.single_thread && SDL_GetAtomicInt(&c->load_complete)) {
                finalize_chunk_load(sys, c);
                if (c->state == JCE_CHUNK_LOADED) {
                    sys->memory_used += c->estimated_size;
                }
            }
            break;

        case JCE_CHUNK_LOADED:
            /* Should we unload this chunk? */
            if (d2 > unload_r2) {
                c->state = JCE_CHUNK_UNLOADING;
                sys->memory_used -= c->estimated_size;
                unload_chunk(sys, c);
                LOG_TRACE(LOG_TAG, "unloaded chunk %u", c->chunk_id);
            }
            break;

        case JCE_CHUNK_UNLOADING:
            /* Unload is synchronous — immediately transition. */
            c->state = JCE_CHUNK_UNLOADED;
            break;
        }

        /* In single-thread mode, respect the frame time budget. */
        if (sys->config.single_thread && loads_this_frame > 0) {
            double elapsed = now_ms() - start;
            if (elapsed >= (double)sys->config.frame_budget_ms)
                break;  /* resume next frame */
        }
    }
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
