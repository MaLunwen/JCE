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
 *       Loads are queued and could be dispatched to a background
 *       thread (via enkiTS).  The current implementation still
 *       processes loads synchronously on the calling thread, but the
 *       data structure is ready for async extension.
 */

#include <jce/streaming/jce_streaming.h>
#include <jce/core/jce_log.h>

#include <SDL3/SDL_timer.h>

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
} ChunkRecord;

/* ── System struct ─────────────────────────────────────────────────── */

struct JceStreamingSystem {
    JceStreamingConfig config;
    ChunkRecord        chunks[MAX_CHUNKS];
    uint32_t           chunk_count;
    uint64_t           memory_used;
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

/* Simulate loading a chunk (placeholder for real I/O). */
static void load_chunk(ChunkRecord *c)
{
    /* TODO: Replace with actual asset loading via jce_fs_read_all() or
       async I/O.  For now, just transition the state. */
    c->state = JCE_CHUNK_LOADED;
    c->estimated_size = 1024 * 1024;  /* placeholder 1 MiB */
}

/* Simulate unloading a chunk. */
static void unload_chunk(ChunkRecord *c)
{
    c->state = JCE_CHUNK_UNLOADED;
    c->estimated_size = 0;
}

/* ── Create / Destroy ─────────────────────────────────────────────── */

JceStreamingSystem *jce_streaming_create(const JceStreamingConfig *config)
{
    if (!config) return NULL;

    JceStreamingSystem *sys = (JceStreamingSystem *)calloc(1, sizeof(*sys));
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

    /* Unload all loaded chunks. */
    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        if (sys->chunks[i].registered &&
            sys->chunks[i].state == JCE_CHUNK_LOADED) {
            unload_chunk(&sys->chunks[i]);
        }
    }

    free(sys);
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

    if (c->state == JCE_CHUNK_LOADED) {
        sys->memory_used -= c->estimated_size;
        unload_chunk(c);
    }
    c->registered = false;
}

/* ── Per-frame update ─────────────────────────────────────────────── */

void jce_streaming_update(JceStreamingSystem *sys, jce_vec3 camera_pos)
{
    if (!sys) return;

    double start = now_ms();
    float load_r2   = sys->config.load_radius   * sys->config.load_radius;
    float unload_r2 = sys->config.unload_radius  * sys->config.unload_radius;
    uint64_t budget_bytes = (uint64_t)sys->config.budget_mb * 1024ULL * 1024ULL;

    uint32_t loads_this_frame   = 0;
    uint32_t unloads_this_frame = 0;

    for (uint32_t i = 0; i < sys->chunk_count; i++) {
        ChunkRecord *c = &sys->chunks[i];
        if (!c->registered) continue;

        float d2 = dist_sq(camera_pos, c->center);

        switch (c->state) {
        case JCE_CHUNK_UNLOADED:
            /* Should we load this chunk? */
            if (d2 <= load_r2 &&
                loads_this_frame < sys->config.max_pending &&
                (budget_bytes == 0 || sys->memory_used < budget_bytes)) {
                c->state = JCE_CHUNK_LOADING;
                load_chunk(c);
                sys->memory_used += c->estimated_size;
                loads_this_frame++;
                LOG_TRACE(LOG_TAG, "loaded chunk %u", c->chunk_id);
            }
            break;

        case JCE_CHUNK_LOADED:
            /* Should we unload this chunk? */
            if (d2 > unload_r2) {
                c->state = JCE_CHUNK_UNLOADING;
                sys->memory_used -= c->estimated_size;
                unload_chunk(c);
                unloads_this_frame++;
                LOG_TRACE(LOG_TAG, "unloaded chunk %u", c->chunk_id);
            }
            break;

        case JCE_CHUNK_LOADING:
        case JCE_CHUNK_UNLOADING:
            /* In-flight; will resolve next frame. */
            break;
        }

        /* In single-thread mode, respect the frame time budget. */
        if (sys->config.single_thread && loads_this_frame > 0) {
            double elapsed = now_ms() - start;
            if (elapsed >= (double)sys->config.frame_budget_ms)
                break;  /* resume next frame */
        }
    }
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

bool jce_streaming_is_single_thread(const JceStreamingSystem *sys)
{
    return sys ? sys->config.single_thread : false;
}
