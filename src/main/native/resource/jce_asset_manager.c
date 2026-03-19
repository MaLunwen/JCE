/*
 * jce_asset_manager.c  Asset manager implementation.
 *
 * Internal table of (path_hash  slot) entries.
 * Linear scan is fine for < 256 assets. A hash map can be added later.
 *
 * NOTE: This module intentionally depends on renderer/jce_texture.h and
 * renderer/jce_mesh.h because its purpose is to manage GPU-backed assets.
 * A vtable/callback abstraction is not justified at this scale.
 */

#include "jce_asset_manager.h"
#include "pak_loader.h"
#include "renderer/jce_texture.h"
#include "renderer/jce_mesh.h"
#include "core/jce_log.h"

#include <SDL3/SDL.h>
#include <xxhash.h>
#include <string.h>

#define LOG_TAG "jce_assets"
#define MAX_ASSETS 256

typedef struct {
    uint64_t       path_hash;
    JceAssetType   type;
    uint32_t       ref_count;
    void          *data;          /* type-specific payload */
    bool           occupied;
} AssetSlot;

struct JceAssetManager {
    PakArchive *pak;
    AssetSlot   slots[MAX_ASSETS];
    uint32_t    count;
};

static uint64_t hash_path(const char *path)
{
    return XXH3_64bits(path, strlen(path));
}

/* Find an existing slot by path hash, or return -1. */
static int find_slot(const JceAssetManager *mgr, uint64_t h)
{
    for (uint32_t i = 0; i < MAX_ASSETS; i++) {
        if (mgr->slots[i].occupied && mgr->slots[i].path_hash == h)
            return (int)i;
    }
    return -1;
}

/* Find a free slot, or return -1. */
static int alloc_slot(JceAssetManager *mgr)
{
    for (uint32_t i = 0; i < MAX_ASSETS; i++) {
        if (!mgr->slots[i].occupied)
            return (int)i;
    }
    return -1;
}

static void free_asset(AssetSlot *slot)
{
    if (!slot || !slot->occupied) return;

    switch (slot->type) {
    case JCE_ASSET_TEXTURE: {
        JceTexture *tex = (JceTexture *)slot->data;
        if (tex) { jce_texture_destroy(*tex); SDL_free(tex); }
        break;
    }
    case JCE_ASSET_MESH: {
        JceMesh *mesh = (JceMesh *)slot->data;
        if (mesh) jce_mesh_destroy(mesh);
        break;
    }
    case JCE_ASSET_SOUND:
    case JCE_ASSET_FONT:
        /* TODO: add destroy calls when sound/font managers are integrated. */
        break;
    }

    slot->data      = NULL;
    slot->occupied  = false;
    slot->ref_count = 0;
}

/* -- Public API ----------------------------------------------------- */

JceAssetManager *jce_asset_manager_create(PakArchive *pak)
{
    if (!pak) return NULL;
    JceAssetManager *mgr = (JceAssetManager *)SDL_calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->pak = pak;
    return mgr;
}

void jce_asset_manager_destroy(JceAssetManager *mgr)
{
    if (!mgr) return;
    for (uint32_t i = 0; i < MAX_ASSETS; i++) {
        if (mgr->slots[i].occupied)
            free_asset(&mgr->slots[i]);
    }
    SDL_free(mgr);
}

JceAssetHandle jce_asset_acquire(JceAssetManager *mgr,
                                  const char *asset_path,
                                  JceAssetType type)
{
    if (!mgr || !asset_path) return JCE_ASSET_INVALID;

    uint64_t h = hash_path(asset_path);

    /* Check if already loaded. */
    int idx = find_slot(mgr, h);
    if (idx >= 0) {
        mgr->slots[idx].ref_count++;
        return (JceAssetHandle){ (uint16_t)idx };
    }

    /* Allocate a new slot. */
    idx = alloc_slot(mgr);
    if (idx < 0) {
        LOG_ERROR(LOG_TAG, "asset table full (%d/%d)", MAX_ASSETS, MAX_ASSETS);
        return JCE_ASSET_INVALID;
    }

    /* Load based on type. */
    void *data = NULL;

    switch (type) {
    case JCE_ASSET_TEXTURE: {
        JceTexture tex = jce_texture_load(mgr->pak, asset_path);
        if (!jce_texture_valid(tex)) {
            LOG_ERROR(LOG_TAG, "failed to load texture: %s", asset_path);
            return JCE_ASSET_INVALID;
        }
        JceTexture *heap_tex = (JceTexture *)SDL_malloc(sizeof(JceTexture));
        if (!heap_tex) { jce_texture_destroy(tex); return JCE_ASSET_INVALID; }
        *heap_tex = tex;
        data = heap_tex;
        break;
    }
    case JCE_ASSET_MESH: {
        JceMesh *mesh = jce_mesh_load(mgr->pak, asset_path);
        if (!mesh) {
            LOG_ERROR(LOG_TAG, "failed to load mesh: %s", asset_path);
            return JCE_ASSET_INVALID;
        }
        data = mesh;
        break;
    }
    case JCE_ASSET_SOUND:
    case JCE_ASSET_FONT:
        LOG_WARN(LOG_TAG, "asset type %d not yet supported via manager", type);
        return JCE_ASSET_INVALID;
    }

    AssetSlot *slot = &mgr->slots[idx];
    slot->path_hash = h;
    slot->type      = type;
    slot->ref_count = 1;
    slot->data      = data;
    slot->occupied  = true;
    mgr->count++;

    return (JceAssetHandle){ (uint16_t)idx };
}

void jce_asset_release(JceAssetManager *mgr, JceAssetHandle handle)
{
    if (!mgr || handle.idx >= MAX_ASSETS) return;
    AssetSlot *slot = &mgr->slots[handle.idx];
    if (!slot->occupied) return;

    if (slot->ref_count > 1) {
        slot->ref_count--;
        return;
    }

    free_asset(slot);
    mgr->count--;
}

void *jce_asset_get_data(const JceAssetManager *mgr, JceAssetHandle handle)
{
    if (!mgr || handle.idx >= MAX_ASSETS) return NULL;
    const AssetSlot *slot = &mgr->slots[handle.idx];
    return slot->occupied ? slot->data : NULL;
}

uint32_t jce_asset_ref_count(const JceAssetManager *mgr, JceAssetHandle handle)
{
    if (!mgr || handle.idx >= MAX_ASSETS) return 0;
    const AssetSlot *slot = &mgr->slots[handle.idx];
    return slot->occupied ? slot->ref_count : 0;
}

uint32_t jce_asset_manager_count(const JceAssetManager *mgr)
{
    return mgr ? mgr->count : 0;
}
