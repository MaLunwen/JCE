/*
 * jce_asset_manager.h  Internal header for the unified asset manager.
 *
 * The public API is in <jce/resource/jce_asset.h>.
 * This header exposes internals needed by the implementation only.
 */

#ifndef JCE_ASSET_MANAGER_NEW_H
#define JCE_ASSET_MANAGER_NEW_H

#include <jce/resource/jce_asset.h>

#include "jce_asset_registry.h"
#include "jce_async_pool.h"

#include <SDL3/SDL_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Slot layout                                                         */
/* ================================================================== */

/* Atomic slot accessors.  `state` and `ref_count` are stored as
 * SDL_AtomicInt so background worker threads can safely publish
 * finished slots and so external observers (debug HUD, hot-reload
 * watcher) can read state without holding the manager lock.
 *
 * SDL3 atomic getters take non-const pointers; we cast away const in
 * the read-only macros because atomic-int reads don't mutate anything
 * observable from C's perspective. */
#define JCE_SLOT_STATE_GET(s)    ((JceAssetState)SDL_GetAtomicInt((SDL_AtomicInt *)&(s)->state))
#define JCE_SLOT_STATE_SET(s, v) SDL_SetAtomicInt(&(s)->state, (int)(v))
#define JCE_SLOT_REF_GET(s)      SDL_GetAtomicInt((SDL_AtomicInt *)&(s)->ref_count)
#define JCE_SLOT_REF_INC(s)      SDL_AddAtomicInt(&(s)->ref_count, 1)
#define JCE_SLOT_REF_DEC(s)      SDL_AddAtomicInt(&(s)->ref_count, -1)
#define JCE_SLOT_REF_SET(s, v)   SDL_SetAtomicInt(&(s)->ref_count, (v))

typedef struct JceAssetSlot {
    uint64_t      path_hash;      /* XXH3_64 of asset path */
    char         *path;           /* owned copy of source asset path */
    JceAssetType  type;
    SDL_AtomicInt state;          /* JceAssetState — use JCE_SLOT_STATE_* */
    uint16_t      generation;     /* generation counter for this slot */
    SDL_AtomicInt ref_count;      /* — use JCE_SLOT_REF_* */
    void         *data;           /* type-specific payload */
    size_t        memory_bytes;   /* approximate memory usage */
    JceAssetLoadParams load_params; /* reload parameters (sanitized copy) */
} JceAssetSlot;

/* ================================================================== */
/* Manager struct                                                      */
/* ================================================================== */

struct JceAssetManager {
    /* Configuration (immutable after creation). */
    JcePakArchive     *pak;
    JceFileSystem  *fs;
    JceAudio       *audio;
    uint32_t        max_assets;

    /* Slot pool: indexed by handle.index. */
    JceAssetSlot   *slots;

    /* Free-list: stack of available slot indices. */
    uint16_t       *free_list;
    uint32_t        free_count;

    /* Hash map: path_hash → slot index. */
    JceAssetRegistry registry;

    /* Async thread pool. */
    JceAsyncPool   *pool;

    /* Pluggable loaders (indexed by JceAssetType).
       NULL entries fall through to the built-in switch. */
    jce_asset_load_fn    ext_loaders[JCE_ASSET_TYPE_COUNT];
    jce_asset_destroy_fn ext_destroyers[JCE_ASSET_TYPE_COUNT];

    /* Statistics. */
    uint32_t        total_loaded;
    uint32_t        failed_loads;
    uint64_t        total_memory;

    /* Optional load-failure callback (set via jce_asset_set_error_handler).
       Invoked from finalize / sync paths whenever an asset transitions
       to JCE_ASSET_STATE_FAILED.  Always called on the main thread. */
    jce_asset_error_fn error_fn;
    void              *error_user;
};

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_MANAGER_NEW_H */
