/*
 * jce_asset_manager.c  Unified asset manager implementation.
 *
 * Core design:
 *   - Generational handle pool: O(1) alloc/free via free-list stack
 *   - Hash-map registry: O(1) path→slot lookup (supports >5000 assets)
 *   - Async thread pool: decompress+decode off main thread
 *   - Frame pump: main thread finalizes GPU resources in time budget
 *   - Reference counting: duplicate loads bump ref count
 *   - Type-safe accessors with generation validation
 *
 * Thread safety:
 *   - All public API calls must be on the main thread
 *   - Only the async pool uses worker threads internally
 */

#include "jce_asset_manager.h"

#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_timer.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_asset_format.h>

#include "jce_asset_loaders.h"
#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <string.h>
#include <xxhash.h>

#define LOG_TAG "jce_asset"
#define DEFAULT_MAX_ASSETS 8192

static void fire_asset_error(JceAssetManager *mgr,
                             const JceAssetSlot *slot,
                             uint16_t slot_index,
                             JceAssetErrorCode code,
                             const char *detail);

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static uint64_t hash_path(const char *path)
{
    return XXH3_64bits(path, strlen(path));
}

static JceAssetLoadParams sanitize_load_params(const JceAssetLoadParams *params)
{
    JceAssetLoadParams clean = JCE_ASSET_LOAD_DEFAULT;
    if (!params) return clean;

    clean.texture_sampler_mode = params->texture_sampler_mode;
    clean.font_size = params->font_size;
    clean.sync = params->sync;
    /* Persist only POD values. Callers provide transient codepoint buffers. */
    clean.font_codepoints = NULL;
    clean.font_codepoint_count = 0;
    return clean;
}

static char *dup_asset_path(const char *path)
{
    if (!path || path[0] == '\0') return NULL;

    size_t len = strlen(path);
    char *copy = (char *)JCE_MALLOC(len + 1);
    if (!copy) return NULL;

    memcpy(copy, path, len + 1);
    return copy;
}

/* Hash that incorporates both path and load params (e.g. sampler mode)
   so the same file with different parameters occupies different slots. */
static uint64_t hash_asset_key(const char *path, JceAssetType type,
                               const JceAssetLoadParams *params)
{
    /* Build a combined buffer: path + type + relevant params. */
    size_t path_len = strlen(path);
    struct {
        uint32_t type;
        int      sampler;
        float    font_size;
    } suffix;
    suffix.type = (uint32_t)type;
    suffix.sampler = (params) ? params->texture_sampler_mode : 0;
    suffix.font_size = (params) ? params->font_size : 0.0f;

    /* Two-step: hash path then mix with suffix. */
    uint64_t h1 = XXH3_64bits(path, path_len);
    uint64_t h2 = XXH3_64bits(&suffix, sizeof(suffix));
    /* Combine using a simple mix. */
    return h1 ^ (h2 * 0x9E3779B97F4A7C15ULL);
}

static bool validate_handle(const JceAssetManager *mgr, JceAssetHandle h)
{
    if (h.index >= mgr->max_assets) return false;
    const JceAssetSlot *slot = &mgr->slots[h.index];
    return slot->generation == h.generation && JCE_SLOT_REF_GET(slot) > 0;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceAssetManager *jce_asset_manager_create(const JceAssetManagerConfig *cfg)
{
    if (!cfg || !cfg->pak) return NULL;

    JceAssetManager *mgr = JCE_NEW(JceAssetManager);
    if (!mgr) return NULL;

    mgr->pak   = cfg->pak;
    mgr->fs    = cfg->fs;
    mgr->audio = cfg->audio;

    /* Slot pool size. */
    uint32_t max_assets = cfg->max_assets;
    if (max_assets == 0) max_assets = DEFAULT_MAX_ASSETS;
    if (max_assets > 0xFFFE) max_assets = 0xFFFE; /* 0xFFFF = invalid */
    mgr->max_assets = max_assets;

    /* Allocate slot pool. */
    mgr->slots = JCE_NEW_ARRAY(JceAssetSlot, max_assets);
    if (!mgr->slots) {
        JCE_FREE(mgr);
        return NULL;
    }

    /* Build free-list (stack, top = highest index for LIFO reuse). */
    mgr->free_list = JCE_NEW_ARRAY(uint16_t, max_assets);
    if (!mgr->free_list) {
        JCE_FREE(mgr->slots);
        JCE_FREE(mgr);
        return NULL;
    }
    for (uint32_t i = 0; i < max_assets; i++)
        mgr->free_list[i] = (uint16_t)(max_assets - 1 - i);
    mgr->free_count = max_assets;

    /* Create hash map registry. */
    uint32_t reg_capacity = max_assets * 2; /* 50% load factor */
    if (!jce_registry_create(&mgr->registry, reg_capacity)) {
        JCE_FREE(mgr->free_list);
        JCE_FREE(mgr->slots);
        JCE_FREE(mgr);
        return NULL;
    }

    /* Create async thread pool. */
    mgr->pool = jce_pool_create(cfg->worker_threads);
    if (!mgr->pool) {
        LOG_WARN(LOG_TAG, "async pool creation failed — sync-only mode");
    }

    LOG_INFO(LOG_TAG, "asset manager: %u slots, registry cap %u",
             max_assets, reg_capacity);
    return mgr;
}

/* ================================================================== */
/* Slot management                                                     */
/* ================================================================== */

static uint16_t alloc_slot(JceAssetManager *mgr)
{
    if (mgr->free_count == 0) return 0xFFFF;
    return mgr->free_list[--mgr->free_count];
}

static void free_slot(JceAssetManager *mgr, uint16_t index)
{
    JceAssetSlot *slot = &mgr->slots[index];
    size_t memory_bytes = slot->memory_bytes;

    destroy_slot_payload(mgr, slot);

    if (slot->path) {
        JCE_FREE(slot->path);
        slot->path = NULL;
    }

    if (mgr->total_memory >= memory_bytes)
        mgr->total_memory -= memory_bytes;
    else
        mgr->total_memory = 0;

    /* Remove from registry. */
    jce_registry_remove(&mgr->registry, slot->path_hash);

    /* Increment generation to invalidate outstanding handles. */
    uint16_t gen = slot->generation;
    memset(slot, 0, sizeof(*slot));
    slot->generation = gen + 1;

    /* Return slot to free-list. */
    mgr->free_list[mgr->free_count++] = index;
}

/* ================================================================== */
/* Public API: create / destroy                                        */
/* ================================================================== */

/* (jce_asset_manager_create defined above) */

void jce_asset_manager_destroy(JceAssetManager *mgr)
{
    if (!mgr) return;

    /* Destroy async pool first (joins worker threads). */
    jce_pool_destroy(mgr->pool);

    /* Free all occupied slots. */
    for (uint32_t i = 0; i < mgr->max_assets; i++) {
        if (JCE_SLOT_REF_GET(&mgr->slots[i]) > 0)
            free_slot(mgr, (uint16_t)i);
    }

    jce_registry_destroy(&mgr->registry);
    JCE_FREE(mgr->free_list);
    JCE_FREE(mgr->slots);
    JCE_FREE(mgr);
}

/* ================================================================== */
/* Public API: acquire / load / release / reload                       */
/* ================================================================== */

static JceAsyncRequestType asset_type_to_async(JceAssetType type)
{
    switch (type) {
    case JCE_ASSET_TEXTURE:   return JCE_ASYNC_TEXTURE;
    case JCE_ASSET_SOUND:     return JCE_ASYNC_AUDIO;
    case JCE_ASSET_MESH:      return JCE_ASYNC_MESH;
    case JCE_ASSET_MODEL:     return JCE_ASYNC_MODEL;
    case JCE_ASSET_FONT:      return JCE_ASYNC_FONT;
    default:                  return JCE_ASYNC_RAW;
    }
}

JceAssetHandle jce_asset_acquire(JceAssetManager *mgr,
                                 const char *asset_path,
                                 JceAssetType type,
                                 const JceAssetLoadParams *params)
{
    if (!mgr || !asset_path) return JCE_ASSET_HANDLE_INVALID;

    uint64_t h = hash_asset_key(asset_path, type, params);

    /* Check if already loaded. */
    uint16_t existing = jce_registry_find(&mgr->registry, h);
    if (existing != UINT16_MAX) {
        JceAssetSlot *slot = &mgr->slots[existing];
        JCE_SLOT_REF_INC(slot);
        return (JceAssetHandle){ existing, slot->generation };
    }

    /* Allocate a new slot. */
    uint16_t idx = alloc_slot(mgr);
    if (idx == 0xFFFF) {
        LOG_ERROR(LOG_TAG, "slot pool full (%u/%u)", mgr->max_assets,
                  mgr->max_assets);
        return JCE_ASSET_HANDLE_INVALID;
    }

    JceAssetSlot *slot = &mgr->slots[idx];
    char *path_copy = dup_asset_path(asset_path);
    if (!path_copy) {
        LOG_ERROR(LOG_TAG, "asset acquire: out of memory for path '%s'", asset_path);
        mgr->free_list[mgr->free_count++] = idx;
        return JCE_ASSET_HANDLE_INVALID;
    }

    slot->path_hash  = h;
    slot->path       = path_copy;
    slot->type       = type;
    JCE_SLOT_REF_SET(slot, 1);
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_UNLOADED);
    slot->data       = NULL;
    slot->memory_bytes = 0;
    slot->load_params = sanitize_load_params(params);

    /* Register in hash map. */
    uint16_t inserted = jce_registry_insert(&mgr->registry, h, idx);
    if (inserted == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "asset acquire failed: registry insert failed (%s)",
                  asset_path);
        JCE_FREE(slot->path);
        slot->path = NULL;
        slot->path_hash = 0;
        slot->type = JCE_ASSET_RAW;
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_UNLOADED);
        JCE_SLOT_REF_SET(slot, 0);
        slot->data = NULL;
        slot->memory_bytes = 0;
        slot->load_params = JCE_ASSET_LOAD_DEFAULT;
        mgr->free_list[mgr->free_count++] = idx;
        return JCE_ASSET_HANDLE_INVALID;
    }

    if (inserted != idx) {
        /* Registry already tracks this key; recycle the speculative slot. */
        JceAssetSlot *existing_slot = &mgr->slots[inserted];
        JCE_SLOT_REF_INC(existing_slot);

        JCE_FREE(slot->path);
        slot->path = NULL;
        slot->path_hash = 0;
        slot->type = JCE_ASSET_RAW;
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_UNLOADED);
        JCE_SLOT_REF_SET(slot, 0);
        slot->data = NULL;
        slot->memory_bytes = 0;
        slot->load_params = JCE_ASSET_LOAD_DEFAULT;
        mgr->free_list[mgr->free_count++] = idx;

        return (JceAssetHandle){ inserted, existing_slot->generation };
    }

    JceAssetHandle handle = { idx, slot->generation };

    /* Decide sync vs async. */
    bool sync = !params || params->sync || !mgr->pool;
    /* Fonts, meshes, and models currently require main-thread for GPU
       upload — load synchronously for now. Textures and audio can async. */
    if (type == JCE_ASSET_MESH || type == JCE_ASSET_MODEL ||
        type == JCE_ASSET_FONT || type == JCE_ASSET_SHADER ||
        type == JCE_ASSET_MATERIAL || type == JCE_ASSET_SCENE)
        sync = true;

    if (sync) {
        load_slot_sync(mgr, slot, asset_path, type, params);

        if (JCE_SLOT_STATE_GET(slot) == JCE_ASSET_STATE_READY) {
            mgr->total_loaded++;
            mgr->total_memory += slot->memory_bytes;
        } else {
            mgr->failed_loads++;
            fire_asset_error(mgr, slot, idx, JCE_ASSET_ERR_INTERNAL,
                             "sync load returned non-READY state");
        }
    } else {
        /* Submit to async pool. */
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_QUEUED);

        JceAsyncLoadInfo info = {0};
        if (params) {
            info.texture_sampler_mode = params->texture_sampler_mode;
            info.font_size = params->font_size;
            if (params->font_codepoints && params->font_codepoint_count > 0) {
                int n = params->font_codepoint_count;
                if (n > 256) n = 256;
                memcpy(info.font_codepoints, params->font_codepoints,
                       (size_t)n * sizeof(uint32_t));
                info.font_codepoint_count = n;
            }
        }

        jce_pool_submit(mgr->pool, asset_type_to_async(type),
                        idx, asset_path, mgr->pak, mgr->fs, &info);
    }

    return handle;
}

JceAssetHandle jce_asset_load(JceAssetManager *mgr,
                              const char *asset_path,
                              JceAssetType type)
{
    JceAssetLoadParams params = JCE_ASSET_LOAD_DEFAULT;
    params.sync = true;
    return jce_asset_acquire(mgr, asset_path, type, &params);
}

void jce_asset_release(JceAssetManager *mgr, JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return;

    JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_REF_GET(slot) > 1) {
        JCE_SLOT_REF_DEC(slot);
        return;
    }

    if (JCE_SLOT_STATE_GET(slot) == JCE_ASSET_STATE_READY && mgr->total_loaded > 0)
        mgr->total_loaded--;
    free_slot(mgr, handle.index);
}

void jce_asset_reload(JceAssetManager *mgr, JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) {
        LOG_WARN(LOG_TAG, "reload ignored: invalid handle");
        return;
    }

    JceAssetSlot *slot = &mgr->slots[handle.index];
    if (!slot->path || slot->path[0] == '\0') {
        LOG_WARN(LOG_TAG, "reload ignored: slot %u has no source path",
                 (unsigned)handle.index);
        return;
    }

    if (JCE_SLOT_STATE_GET(slot) == JCE_ASSET_STATE_QUEUED ||
        JCE_SLOT_STATE_GET(slot) == JCE_ASSET_STATE_LOADING) {
        LOG_WARN(LOG_TAG, "reload deferred: asset still loading (%s)", slot->path);
        return;
    }

    JceAssetLoadParams params = slot->load_params;
    params.sync = true;
    params.font_codepoints = NULL;
    params.font_codepoint_count = 0;

    JceAssetSlot staged;
    memset(&staged, 0, sizeof(staged));
    staged.type = slot->type;
    load_slot_sync(mgr, &staged, slot->path, slot->type, &params);

    if (JCE_SLOT_STATE_GET(&staged) != JCE_ASSET_STATE_READY) {
        destroy_slot_payload(mgr, &staged);
        mgr->failed_loads++;
        fire_asset_error(mgr, slot, (uint16_t)handle.index,
                         JCE_ASSET_ERR_INTERNAL, "reload failed");
        LOG_WARN(LOG_TAG, "reload failed, keeping previous asset data: %s",
                 slot->path);
        return;
    }

    JceAssetSlot old_payload;
    memset(&old_payload, 0, sizeof(old_payload));
    old_payload.type = slot->type;
    old_payload.data = slot->data;
    old_payload.memory_bytes = slot->memory_bytes;

    if (JCE_SLOT_STATE_GET(slot) == JCE_ASSET_STATE_READY) {
        if (mgr->total_memory >= slot->memory_bytes)
            mgr->total_memory -= slot->memory_bytes;
        else
            mgr->total_memory = 0;
    }

    slot->data = staged.data;
    slot->memory_bytes = staged.memory_bytes;
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);

    staged.data = NULL;
    staged.memory_bytes = 0;
    destroy_slot_payload(mgr, &old_payload);

    mgr->total_memory += slot->memory_bytes;

    LOG_INFO(LOG_TAG, "asset reloaded: %s (slot=%u)",
             slot->path, (unsigned)handle.index);
}

/* ================================================================== */
/* Public API: query                                                   */
/* ================================================================== */

JceAssetState jce_asset_state(const JceAssetManager *mgr,
                              JceAssetHandle handle)
{
    if (!mgr || handle.index >= mgr->max_assets) return JCE_ASSET_STATE_UNLOADED;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (slot->generation != handle.generation) return JCE_ASSET_STATE_UNLOADED;
    return JCE_SLOT_STATE_GET(slot);
}

uint32_t jce_asset_ref_count(const JceAssetManager *mgr,
                             JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return 0;
    return (uint32_t)JCE_SLOT_REF_GET(&mgr->slots[handle.index]);
}

JceAssetType jce_asset_type(const JceAssetManager *mgr,
                            JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return JCE_ASSET_RAW;
    return mgr->slots[handle.index].type;
}

uint32_t jce_asset_count(const JceAssetManager *mgr)
{
    return mgr ? mgr->total_loaded : 0;
}

/* ================================================================== */
/* Public API: data retrieval                                          */
/* ================================================================== */

void *jce_asset_data(const JceAssetManager *mgr, JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return NULL;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_STATE_GET(slot) != JCE_ASSET_STATE_READY) return NULL;
    return slot->data;
}

JceTexture jce_asset_get_texture(const JceAssetManager *mgr,
                                 JceAssetHandle handle)
{
    JceTexture invalid = { UINT16_MAX };
    if (!mgr || !validate_handle(mgr, handle)) return invalid;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_STATE_GET(slot) != JCE_ASSET_STATE_READY ||
        slot->type != JCE_ASSET_TEXTURE || !slot->data)
        return invalid;
    return *(JceTexture *)slot->data;
}

JceMesh *jce_asset_get_mesh(const JceAssetManager *mgr,
                            JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return NULL;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_STATE_GET(slot) != JCE_ASSET_STATE_READY ||
        slot->type != JCE_ASSET_MESH)
        return NULL;
    return (JceMesh *)slot->data;
}

JceModel *jce_asset_get_model(const JceAssetManager *mgr,
                              JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return NULL;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_STATE_GET(slot) != JCE_ASSET_STATE_READY ||
        slot->type != JCE_ASSET_MODEL)
        return NULL;
    return (JceModel *)slot->data;
}

JceSound jce_asset_get_sound(const JceAssetManager *mgr,
                             JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return JCE_SOUND_INVALID;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_STATE_GET(slot) != JCE_ASSET_STATE_READY ||
        slot->type != JCE_ASSET_SOUND || !slot->data)
        return JCE_SOUND_INVALID;
    return *(JceSound *)slot->data;
}

JceFont *jce_asset_get_font(const JceAssetManager *mgr,
                            JceAssetHandle handle)
{
    if (!mgr || !validate_handle(mgr, handle)) return NULL;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_STATE_GET(slot) != JCE_ASSET_STATE_READY ||
        slot->type != JCE_ASSET_FONT)
        return NULL;
    return (JceFont *)slot->data;
}

const void *jce_asset_get_raw(const JceAssetManager *mgr,
                              JceAssetHandle handle,
                              size_t *out_size)
{
    if (out_size) *out_size = 0;
    if (!mgr || !validate_handle(mgr, handle)) return NULL;
    const JceAssetSlot *slot = &mgr->slots[handle.index];
    if (JCE_SLOT_STATE_GET(slot) != JCE_ASSET_STATE_READY || !slot->data)
        return NULL;
    if (out_size) *out_size = slot->memory_bytes;
    return slot->data;
}

/* ================================================================== */
/* Frame pump                                                          */
/* ================================================================== */

uint32_t jce_asset_manager_update(JceAssetManager *mgr,
                                  float max_finalize_ms)
{
    JCE_PROFILE_ZONE_N("AssetManager::Update");
    if (!mgr || !mgr->pool) {
        JCE_PROFILE_ZONE_END;
        return 0;
    }

    uint64_t start = jce_time_perf_counter();
    uint64_t freq  = jce_time_perf_freq();
    double budget_sec = (max_finalize_ms > 0)
        ? (double)max_finalize_ms / 1000.0
        : 1e9; /* unlimited */

    uint32_t count = 0;

    /* Finalize in small batches, re-checking the time budget between batches
       and leaving any not-yet-drained requests in the pool's done-list for the
       next frame. This actually honours max_finalize_ms — the old code drained
       everything up front and a "finalize everything" fallback ignored the
       budget, producing multi-ms load-burst hitches. */
    const uint32_t kFinalizeBatch = 8;
    for (;;) {
        if (max_finalize_ms > 0) {
            double elapsed = (double)(jce_time_perf_counter() - start) / (double)freq;
            if (elapsed >= budget_sec)
                break; /* remainder stays queued in the pool for next frame */
        }

        JceAsyncRequest *chain = jce_pool_drain(mgr->pool, kFinalizeBatch);
        if (!chain)
            break; /* nothing left to finalize this frame */

        while (chain) {
            JceAsyncRequest *req = chain;
            chain = req->next;
            req->next = NULL;

            /* Validate slot is still expecting this load. */
            if (req->slot_index < mgr->max_assets) {
                JceAssetSlot *slot = &mgr->slots[req->slot_index];

                if (req->success) {
                    switch (req->type) {
                    case JCE_ASYNC_TEXTURE: finalize_texture(mgr, slot, req); break;
                    case JCE_ASYNC_AUDIO:   finalize_audio(mgr, slot, req);   break;
                    default:                finalize_raw(mgr, slot, req);     break;
                    }
                } else {
                    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
                    mgr->failed_loads++;
                    LOG_ERROR(LOG_TAG, "async load failed: %s", req->path);
                    fire_asset_error(mgr, slot, (uint16_t)req->slot_index,
                                     JCE_ASSET_ERR_IO, "async pipeline failure");
                }
            }

            jce_pool_free_request(req);
            count++;
        }
    }

#if defined(JCE_PROFILER_ENABLED)
    {
        uint32_t loaded = mgr->max_assets - mgr->free_count;
        JCE_PROFILE_PLOT_I("assets.loaded", (int64_t)loaded);
        JCE_PROFILE_PLOT_I("assets.memory_mb", (int64_t)(mgr->total_memory / (1024 * 1024)));
    }
#endif

    JCE_PROFILE_ZONE_END;
    return count;
}

/* ================================================================== */
/* Statistics                                                          */
/* ================================================================== */

void jce_asset_manager_stats(const JceAssetManager *mgr,
                             JceAssetStats *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!mgr) return;

    out->total_loaded        = mgr->total_loaded;
    out->total_slots_used    = mgr->max_assets - mgr->free_count;
    out->total_slots_capacity = mgr->max_assets;
    out->pending_loads       = jce_pool_pending_count(mgr->pool);
    out->failed_loads        = mgr->failed_loads;
    out->total_memory_bytes  = mgr->total_memory;
}

/* ================================================================== */
/* Pluggable loader registration                                       */
/* ================================================================== */

static void fire_asset_error(JceAssetManager *mgr,
                             const JceAssetSlot *slot,
                             uint16_t slot_index,
                             JceAssetErrorCode code,
                             const char *detail)
{
    if (!mgr || !mgr->error_fn) return;
    JceAssetErrorInfo info;
    info.handle.index      = slot_index;
    info.handle.generation = slot ? slot->generation : 0;
    info.type              = slot ? (JceAssetType)slot->type : JCE_ASSET_RAW;
    info.code              = code;
    info.path              = (slot && slot->path) ? slot->path : "<unknown>";
    info.detail            = detail;
    mgr->error_fn(&info, mgr->error_user);
}

bool jce_asset_register_loader(JceAssetManager *mgr,
                               JceAssetType type,
                               jce_asset_load_fn load_fn,
                               jce_asset_destroy_fn destroy_fn)
{
    if (!mgr || !load_fn) return false;
    if ((uint32_t)type >= JCE_ASSET_TYPE_COUNT) return false;

    if (mgr->ext_loaders[type]) {
        LOG_WARN(LOG_TAG, "overwriting registered loader for type %d", (int)type);
    }

    mgr->ext_loaders[type]    = load_fn;
    mgr->ext_destroyers[type] = destroy_fn;
    LOG_INFO(LOG_TAG, "registered external loader for type %d", (int)type);
    return true;
}

void jce_asset_set_error_handler(JceAssetManager *mgr,
                                 jce_asset_error_fn fn,
                                 void *user)
{
    if (!mgr) return;
    mgr->error_fn   = fn;
    mgr->error_user = user;
}
