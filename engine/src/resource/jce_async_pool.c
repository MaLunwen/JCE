/*
 * jce_async_pool.c  Async asset loading via JceAsyncExecutor.
 *
 * Each submitted request is dispatched to a private structured executor.
 * The main thread drains completed requests each frame via
 * jce_pool_drain().
 *
 * Worker decoding:
 *   TEXTURE → jce_pak_decompress + jce_image (RGBA8) → SDL_Surface
 *   AUDIO   → jce_pak_decompress + jce_audio_decode_cpu_memory → PCM
 *   MESH    → jce_pak_decompress (raw bytes for main-thread GPU upload)
 *   RAW     → jce_pak_decompress (pass-through)
 */

#include "jce_async_pool.h"

#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_sysinfo.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/renderer/jce_image.h>    /* the one image-decode service */
#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_asset_format.h>

#include "jce_asset_reader.h"
#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JCE_NO_AUDIO
#include <jce/middleware/audio/jce_audio.h>
#endif

#define LOG_TAG "jce_pool"

/* Free a request's decoded payload with the allocator that produced it
 * (raw SDL_Surface vs JCE_MALLOC'd buffer).  Defined below; forward-declared
 * here because jce_pool_destroy (above the definition) also uses it. */
static void request_payload_free(JceAsyncRequest *req);

/* ================================================================== */
/* Pool internals                                                      */
/* ================================================================== */

/* A tracked in-flight request. */
typedef struct InFlight {
    JceAsyncRequest *req;
    JceAsyncTask    *task;
    struct InFlight *next;
} InFlight;

struct JceAsyncPool {
    JceAsyncExecutor *executor;

    /* Quiesce flag set during shutdown.  All public APIs (submit, drain,
     * destroy) MUST be called from the same thread (typically the main
     * thread) — workers only touch their own JceAsyncRequest and set the
     * atomic `req->done` flag.  The flag is provided so future code that
     * adds worker callbacks accessing pool state can short-circuit safely. */
    SDL_AtomicInt shutting_down;

    /* In-flight list (mutex-protected). */
    JceMutex  *lock;
    InFlight  *inflight_head;
    uint32_t   inflight_count;

    /* Completed list (mutex-protected, drained by main thread). */
    JceMutex        *done_lock;
    JceAsyncRequest *done_head;
    JceAsyncRequest *done_tail;
};

/* ================================================================== */
/* Worker: texture decode                                              */
/* ================================================================== */

/* Wrap engine-owned RGBA8 pixels in an SDL_Surface.
 *
 * The raw-texture payload contract with the consumer (jce_asset_loaders.c
 * finalize_texture_inner, and request_payload_free below) is "decoded_data is
 * an SDL_Surface*", so the decode result is copied into one here.  It cannot
 * be handed over zero-copy: SDL_CreateSurfaceFrom does not take ownership and
 * an SDL_Surface has no release callback, so SDL_DestroySurface would leak the
 * engine buffer.  One memcpy per decoded texture, on a worker thread. */
static SDL_Surface *surface_from_rgba8(const uint8_t *rgba8, int w, int h)
{
    SDL_Surface *surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    const size_t row_bytes = (size_t)w * 4u;
    for (int y = 0; y < h; y++)
        memcpy((uint8_t *)surf->pixels + (size_t)y * (size_t)surf->pitch,
               rgba8 + (size_t)y * row_bytes, row_bytes);
    return surf;
}

static bool pak_asset_size(const JcePakAsset *asset, const char *path,
                           size_t *out_size)
{
    if (!asset || !out_size || asset->original_size == 0 ||
        asset->original_size > (uint64_t)SIZE_MAX) {
        LOG_ERROR(LOG_TAG, "asset size is not addressable: %s",
                  path ? path : "(null)");
        return false;
    }
    *out_size = (size_t)asset->original_size;
    return true;
}

static void decode_texture_inner(JceAsyncRequest *req)
{
    const JcePakAsset *asset = jce_pak_find(req->pak, req->path);
    size_t asset_size;
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found: %s", req->path);
        return;
    }
    if (!pak_asset_size(asset, req->path, &asset_size))
        return;

    void *buf = JCE_MALLOC(asset_size);
    if (!buf) return;

    size_t n = jce_pak_decompress(asset, buf, asset_size);
    if (n == 0) {
        JCE_FREE(buf);
        LOG_ERROR(LOG_TAG, "decompress failed: %s", req->path);
        return;
    }

    /* ── Cooked path: .jceasset TEX_PIXELS → raw RGBA8 ── */
    if (jce_asset_is_cooked(buf, n)) {
        JceAssetView view;
        if (!jce_asset_open(&view, buf, n)) {
            JCE_FREE(buf);
            return;
        }

        const JceAssetChunkEntry *info_c =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
        const JceAssetChunkEntry *pix_c =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);

        if (!info_c || !pix_c) {
            JCE_FREE(buf);
            return;
        }

        /* Read texture info (info chunk may include mip offsets). */
        if (info_c->original_size < sizeof(JceAssetTexInfo) ||
            info_c->original_size > (uint64_t)SIZE_MAX ||
            pix_c->original_size > (uint64_t)
                (SIZE_MAX - sizeof(JceAssetTexInfo))) {
            JCE_FREE(buf);
            return;
        }

        size_t info_size = (size_t)info_c->original_size;
        void *info_buf = JCE_MALLOC(info_size);
        if (!info_buf) { JCE_FREE(buf); return; }

        if (jce_asset_chunk_data(&view, info_c,
                                  info_buf, info_size) == 0) {
            JCE_FREE(info_buf);
            JCE_FREE(buf);
            return;
        }

        JceAssetTexInfo tex_info;
        memcpy(&tex_info, info_buf, sizeof(tex_info));
        JCE_FREE(info_buf);

        size_t pixel_size = (size_t)pix_c->original_size;
        void *pixels = JCE_MALLOC(pixel_size + sizeof(JceAssetTexInfo));
        if (!pixels) { JCE_FREE(buf); return; }

        memcpy(pixels, &tex_info, sizeof(tex_info));
        if (jce_asset_chunk_data(&view, pix_c,
                                  (uint8_t *)pixels + sizeof(JceAssetTexInfo),
                                  pixel_size) == 0) {
            JCE_FREE(pixels);
            JCE_FREE(buf);
            return;
        }

        JCE_FREE(buf);
        req->decoded_data = pixels;
        req->decoded_size = pixel_size + sizeof(JceAssetTexInfo);
        req->is_cooked = true;
        req->success = true;
        return;
    }

    /* ── Raw path: PNG/JPG → jce_image → RGBA8 → SDL_Surface ──
     *
     * Decoding through the service rather than calling IMG_Load_IO here is
     * what keeps this worker off SDL_image's libpng path for 16-bit grayscale
     * PNGs, which heap-overruns on that class (STATUS_HEAP_CORRUPTION).  The
     * synchronous loader and the cooker already routed around it; this path
     * did not, so a game that streamed such a height map in asynchronously
     * corrupted the heap on a worker thread.  The service also guarantees
     * RGBA8 + a tight stride, so no separate convert step is needed. */
    int img_w = 0, img_h = 0;
    uint8_t *rgba8 = jce_image_load_rgba8_from_memory(
        buf, (uint64_t)asset_size, &img_w, &img_h);
    JCE_FREE(buf);

    if (!rgba8) {
        /* The codec-level reason is logged by the image service itself. */
        LOG_ERROR(LOG_TAG, "image decode failed: %s", req->path);
        return;
    }

    SDL_Surface *surf = surface_from_rgba8(rgba8, img_w, img_h);
    jce_image_free_rgba8(rgba8);
    if (!surf) {
        LOG_ERROR(LOG_TAG, "SDL_CreateSurface failed: %s", req->path);
        return;
    }

    req->decoded_data = surf;
    req->decoded_size = (size_t)img_w * (size_t)img_h * 4u;
    req->success = true;
}

static void decode_texture(JceAsyncRequest *req)
{
    JCE_PROFILE_ZONE_N("Asset::DecodeTexture");
    decode_texture_inner(req);
    JCE_PROFILE_ZONE_END;
}

/* ================================================================== */
/* Worker: audio decode                                                */
/* ================================================================== */

static void decode_audio_inner(JceAsyncRequest *req)
{
#ifdef JCE_NO_AUDIO
    LOG_WARN(LOG_TAG, "audio disabled: %s", req->path);
    return;
#else
    const JcePakAsset *asset = jce_pak_find(req->pak, req->path);
    size_t asset_size;
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found: %s", req->path);
        return;
    }
    if (!pak_asset_size(asset, req->path, &asset_size))
        return;

    void *buf = JCE_MALLOC(asset_size);
    if (!buf) return;

    size_t n = jce_pak_decompress(asset, buf, asset_size);
    if (n == 0) {
        JCE_FREE(buf);
        return;
    }

    typedef struct {
        uint32_t sample_rate;
        uint16_t channels;
        uint16_t bits_per_sample;
        uint32_t pcm_size;
    } AudioResult;

    /* Both representations — a cooked .jceasset (AUDIO_INFO + AUDIO_PCM) and a
     * raw encoded clip — go through the audio module's canonical decoder.  It
     * touches no JceAudio state, so it is safe on this worker thread, and it
     * is the only decode path that has the Opus custom backend and the
     * M4A/AAC route registered: the private ma_decoder this function used to
     * drive silently failed on .opus/.m4a that jce_audio_load() played fine
     * (audit A2-AUDIO-DECODE-DRIFT). */
    bool cooked = jce_asset_is_cooked(buf, n);

    JceAudioCpu *cpu = jce_audio_decode_cpu_memory(buf, n, req->path);
    JCE_FREE(buf);
    if (!cpu) {
        LOG_ERROR(LOG_TAG, "audio decode failed: %s", req->path);
        return;
    }

    const void *pcm         = NULL;
    uint32_t    pcm_size    = 0;
    uint32_t    sample_rate = 0;
    uint16_t    channels    = 0;
    uint16_t    bits        = 0;
    if (!jce_audio_cpu_get_pcm(cpu, &pcm, &pcm_size, &channels,
                               &sample_rate, &bits)) {
        LOG_ERROR(LOG_TAG, "audio decoded to no samples: %s", req->path);
        jce_audio_cpu_free(cpu);
        return;
    }

    /* Copy out of the JceAudioCpu: decoded_data is a single JCE_MALLOC'd block
     * (header + PCM) released by request_payload_free / the loader, whereas
     * `cpu` owns its buffer and must be handed back to jce_audio_cpu_free. */
    AudioResult *result = JCE_MALLOC(sizeof(AudioResult) + pcm_size);
    if (!result) { jce_audio_cpu_free(cpu); return; }

    result->sample_rate     = sample_rate;
    result->channels        = channels;
    result->bits_per_sample = bits;
    result->pcm_size        = pcm_size;
    memcpy((uint8_t *)result + sizeof(AudioResult), pcm, pcm_size);
    jce_audio_cpu_free(cpu);

    req->decoded_data = result;
    req->decoded_size = sizeof(AudioResult) + pcm_size;
    req->is_cooked = cooked;
    req->success = true;
#endif
}

static void decode_audio(JceAsyncRequest *req)
{
    JCE_PROFILE_ZONE_N("Asset::DecodeAudio");
    decode_audio_inner(req);
    JCE_PROFILE_ZONE_END;
}

/* ================================================================== */
/* Worker: raw/mesh decompress                                         */
/* ================================================================== */

static void decode_raw(JceAsyncRequest *req)
{
    const JcePakAsset *asset = jce_pak_find(req->pak, req->path);
    size_t asset_size;
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found: %s", req->path);
        return;
    }
    if (!pak_asset_size(asset, req->path, &asset_size))
        return;

    void *buf = JCE_MALLOC(asset_size);
    if (!buf) return;

    size_t n = jce_pak_decompress(asset, buf, asset_size);
    if (n == 0) {
        JCE_FREE(buf);
        return;
    }

    req->decoded_data = buf;
    req->decoded_size = n;
    req->success = true;
}

/* ================================================================== */
/* Task callback (dispatches by request type)                          */
/* ================================================================== */

static JceAsyncRunResult async_task_fn(JceAsyncContext *ctx, void *arg)
{
    JceAsyncRequest *req = (JceAsyncRequest *)arg;

    if (jce_async_context_cancel_requested(ctx)) {
        SDL_SetAtomicInt(&req->done, 1);
        return JCE_ASYNC_RUN_CANCELLED;
    }

    switch (req->type) {
    case JCE_ASYNC_TEXTURE: decode_texture(req); break;
    case JCE_ASYNC_AUDIO:   decode_audio(req);   break;
    case JCE_ASYNC_MESH:    /* fall through */
    case JCE_ASYNC_MODEL:   /* fall through */
    case JCE_ASYNC_RAW:     /* fall through */
    case JCE_ASYNC_FONT:    decode_raw(req);      break;
    }

    SDL_SetAtomicInt(&req->done, 1);
    return req->success ? JCE_ASYNC_RUN_SUCCESS : JCE_ASYNC_RUN_FAILED;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

JceAsyncPool *jce_pool_create(uint32_t num_workers)
{
    JceAsyncExecutorConfig config;
    JceAsyncExecutorStats stats;
    JceAsyncPool *pool = JCE_NEW(JceAsyncPool);
    if (!pool) return NULL;

    if (num_workers == 0) {
#if JCE_PLATFORM_ANDROID
        num_workers = 2;
#else
        /* Scale with the machine.  A hardcoded 3 left 29 of this box's 32
         * cores idle on the one workload in the engine that is embarrassingly
         * parallel: opening caged_kingdom/hidden_cove needs 49 glTF decodes at
         * ~92 ms each -- 4.5 s of CPU that three workers turn into 1.5 s of
         * wall clock while the first frame is still waiting for it.  Every
         * standard engine sizes its asset-loading pool to the host.
         *
         * cores-1 leaves the main thread a core; the cap of 8 is the same one
         * the shared frame pool uses, and beyond it these decodes are bound by
         * file I/O and allocator contention rather than cores.  LOW machine
         * class keeps the old conservative count -- that tier exists to leave
         * headroom on a 512 MB / few-core box. */
        JceSysInfo si;
        jce_sysinfo_init(&si);
        int cores = si.cpu_cores > 0 ? si.cpu_cores : 4;
        num_workers = (jce_config_machine_class() == JCE_MACHINE_CLASS_LOW)
                          ? 2
                          : (cores > 1 ? cores - 1 : 1);
        if (num_workers > 8) num_workers = 8;
        if (num_workers < 2) num_workers = 2;
#endif
    }

    /* A private executor, deliberately not the shared frame pool. Every
     * task here is a whole-asset decode — zstd inflate, PNG/JPEG decode,
     * Opus/AAC decode — that runs for tens to hundreds of milliseconds.
     * Structured async keeps that work isolated from frame fork/join jobs and
     * supplies bounded submission, cancellation, Web deferral, and teardown. */
    jce_async_executor_config_init(&config);
    config.worker_count = num_workers;
    config.reserve_latency_worker = false;
    config.max_tasks = 1024;
    config.debug_name = "asset-decode";
    pool->executor = jce_async_executor_create(&config);
    if (!pool->executor) {
        JCE_FREE(pool);
        return NULL;
    }

    pool->lock = jce_mutex_create();
    pool->done_lock = jce_mutex_create();
    if (!pool->lock || !pool->done_lock) {
        jce_pool_destroy(pool);
        return NULL;
    }

    jce_async_executor_get_stats(pool->executor, &stats);
    LOG_INFO(LOG_TAG, "async pool: %u workers (%s)", stats.worker_count,
             stats.mode == JCE_ASYNC_EXECUTION_COOPERATIVE
                 ? "cooperative" : "enkiTS");
    return pool;
}

/* Defined below (request teardown section); destroy paths free payloads too. */
static void request_payload_free(JceAsyncRequest *req);

void jce_pool_destroy(JceAsyncPool *pool)
{
    if (!pool) return;

    /* Mark quiesce first so any future worker code that adds a pool-state
     * touch can bail out gracefully.  Today workers only touch their own
     * request struct (atomic `done` flag), so this is purely defensive. */
    SDL_SetAtomicInt(&pool->shutting_down, 1);

    /*
     * Do not start queued decodes while the owning asset manager is
     * shutting down.  Running decoders observe cooperative cancellation;
     * shutdown still waits for their cleanup before request payloads are
     * released below.
     */
    if (pool->executor) {
        (void)jce_async_executor_shutdown(
            pool->executor, JCE_ASYNC_SHUTDOWN_CANCEL_ALL,
            JCE_ASYNC_WAIT_INFINITE);
        jce_async_executor_destroy(pool->executor);
        pool->executor = NULL;
    }

    /* Free in-flight tracking nodes (tasks are already complete).  Take
     * the lock for symmetry with submit/drain even though no other thread
     * can touch the list at this point. */
    if (pool->lock) jce_mutex_lock(pool->lock);
    InFlight *inf = pool->inflight_head;
    pool->inflight_head = NULL;
    pool->inflight_count = 0;
    if (pool->lock) jce_mutex_unlock(pool->lock);
    while (inf) {
        InFlight *next = inf->next;
        if (inf->task) jce_async_task_release(inf->task);
        if (inf->req) {
            request_payload_free(inf->req);
            JCE_FREE(inf->req);
        }
        JCE_FREE(inf);
        inf = next;
    }

    /* Free remaining done requests (drained by main thread only). */
    if (pool->done_lock) jce_mutex_lock(pool->done_lock);
    JceAsyncRequest *req = pool->done_head;
    pool->done_head = NULL;
    pool->done_tail = NULL;
    if (pool->done_lock) jce_mutex_unlock(pool->done_lock);
    while (req) {
        JceAsyncRequest *next = req->next;
        request_payload_free(req);
        JCE_FREE(req);
        req = next;
    }

    if (pool->lock)      jce_mutex_destroy(pool->lock);
    if (pool->done_lock) jce_mutex_destroy(pool->done_lock);
    JCE_FREE(pool);
}

JceAsyncRequest *jce_pool_submit(JceAsyncPool *pool,
                                 JceAsyncRequestType type,
                                 uint16_t slot_index,
                                 uint16_t generation,
                                 const char *path,
                                 JcePakArchive *pak,
                                 JceFileSystem *fs,
                                 const JceAsyncLoadInfo *info)
{
    JceAsyncTaskDesc desc;
    InFlight *inf;
    JceAsyncTask *task;
    if (!pool || !path) return NULL;

    JceAsyncRequest *req = JCE_NEW(JceAsyncRequest);
    if (!req) return NULL;

    req->type       = type;
    req->slot_index = slot_index;
    req->generation = generation;
    req->pak        = pak;
    req->fs         = fs;
    snprintf(req->path, sizeof(req->path), "%s", path);

    if (info)
        req->info = *info;

    /* Allocate bookkeeping before submission so an OOM cannot orphan an
     * already-running request. */
    inf = JCE_NEW(InFlight);
    if (!inf) {
        JCE_FREE(req);
        return NULL;
    }

    jce_async_task_desc_init(&desc);
    desc.work = async_task_fn;
    desc.user_data = req;
    desc.debug_name = path;
    desc.priority = JCE_ASYNC_PRIORITY_LOW;
    task = jce_async_submit(pool->executor, &desc);
    if (!task) {
        JCE_FREE(inf);
        JCE_FREE(req);
        return NULL;
    }

    inf->req  = req;
    inf->task = task;

    jce_mutex_lock(pool->lock);
    inf->next = pool->inflight_head;
    pool->inflight_head = inf;
    pool->inflight_count++;
    jce_mutex_unlock(pool->lock);

    return req;
}

JceAsyncRequest *jce_pool_drain(JceAsyncPool *pool, uint32_t max_count)
{
    JceAsyncPumpBudget budget;
    if (!pool) return NULL;

    /* On native this dispatches owner completions/reaping; in Web's
     * no-pthread mode it starts at most one deferred decode. A single image
     * or model decode can already consume the frame budget. */
    jce_async_pump_budget_init(&budget);
    budget.max_work_items = 1;
    budget.max_completions = UINT32_MAX;
    budget.max_time_us = 0;
    jce_async_executor_pump(pool->executor, &budget);

    /* Scan in-flight list: move completed tasks to done list. */
    jce_mutex_lock(pool->lock);

    InFlight **pp = &pool->inflight_head;
    while (*pp) {
        InFlight *inf = *pp;
        if (jce_async_task_is_terminal(inf->task)) {
            /* Remove from in-flight. */
            *pp = inf->next;
            pool->inflight_count--;

            jce_async_task_release(inf->task);

            /* Push to done list. */
            JceAsyncRequest *req = inf->req;
            req->next = NULL;

            jce_mutex_lock(pool->done_lock);
            if (pool->done_tail)
                pool->done_tail->next = req;
            else
                pool->done_head = req;
            pool->done_tail = req;
            jce_mutex_unlock(pool->done_lock);

            JCE_FREE(inf);
        } else {
            pp = &inf->next;
        }
    }

    jce_mutex_unlock(pool->lock);

    /* Now drain from done list. */
    jce_mutex_lock(pool->done_lock);

    JceAsyncRequest *result = NULL;

    if (max_count == 0 || max_count >= UINT32_MAX) {
        result = pool->done_head;
        pool->done_head = NULL;
        pool->done_tail = NULL;
    } else {
        JceAsyncRequest *head = pool->done_head;
        JceAsyncRequest *tail = NULL;
        uint32_t count = 0;

        JceAsyncRequest *cur = head;
        while (cur && count < max_count) {
            tail = cur;
            cur = cur->next;
            count++;
        }

        if (tail) {
            result = head;
            tail->next = NULL;
            pool->done_head = cur;
            if (!cur) pool->done_tail = NULL;
        }
    }

    jce_mutex_unlock(pool->done_lock);
    return result;
}

/* Free a request's decoded payload with the allocator that produced it.
 * The RAW texture path stores an SDL_Surface* (SDL heap) in decoded_data
 * (decode_texture_inner: surface_from_rgba8); every other path — cooked texture,
 * audio, mesh, model — stores a JCE_MALLOC'd buffer.  Releasing an
 * SDL_Surface with JCE_FREE (mi_free) is a cross-allocator free: it
 * corrupts the heap wherever the SDL->jce alloc bridge is absent (the
 * editor never installs jce_alloc_hook_sdl), and even when the bridge IS
 * active it leaks surface->pixels and the surface's SDL properties.
 * jce_pool_free_request / jce_pool_destroy reach this on the drop paths
 * (stale-generation result discarded on scene switch, pool teardown with
 * loads in flight); only finalize_texture_inner freed it correctly before. */
static void request_payload_free(JceAsyncRequest *req)
{
    if (!req || !req->decoded_data) return;
    if (req->type == JCE_ASYNC_TEXTURE && !req->is_cooked)
        SDL_DestroySurface((SDL_Surface *)req->decoded_data);
    else
        JCE_FREE(req->decoded_data);
    req->decoded_data = NULL;
}

void jce_pool_free_request(JceAsyncRequest *req)
{
    if (!req) return;
    request_payload_free(req);
    JCE_FREE(req);
}

uint32_t jce_pool_pending_count(const JceAsyncPool *pool)
{
    uint32_t count;
    if (!pool) return 0;
    jce_mutex_lock(pool->lock);
    count = pool->inflight_count;
    jce_mutex_unlock(pool->lock);
    return count;
}
