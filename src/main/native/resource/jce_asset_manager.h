/*
 * jce_asset_manager.h  Centralized asset manager with reference counting.
 *
 * Prevents duplicate loads: requesting the same PAK path twice returns
 * the same handle and bumps the ref count. Release decrements it;
 * the asset is freed when the count drops to zero.
 */

#ifndef JCE_ASSET_MANAGER_H
#define JCE_ASSET_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceAssetManager JceAssetManager;
typedef struct PakArchive      PakArchive;
typedef struct JceRenderer     JceRenderer;

/* Asset type tags. */
typedef enum {
    JCE_ASSET_TEXTURE,
    JCE_ASSET_MESH,
    JCE_ASSET_SOUND,
    JCE_ASSET_FONT
} JceAssetType;

/* Opaque asset handle (index into internal table). */
typedef struct { uint16_t idx; } JceAssetHandle;
#define JCE_ASSET_INVALID ((JceAssetHandle){ UINT16_MAX })

static inline bool jce_asset_valid(JceAssetHandle h) {
    return h.idx != UINT16_MAX;
}

/* Create the asset manager. */
JceAssetManager *jce_asset_manager_create(PakArchive *pak);

/* Destroy the asset manager and release all remaining assets. */
void jce_asset_manager_destroy(JceAssetManager *mgr);

/* Acquire an asset. If already loaded, bumps ref count.
   Otherwise loads from PAK. Returns INVALID on failure.
   For TEXTURE type, the data pointer is a JceTexture*.
   For MESH type, the data pointer is a JceMesh*.
   The caller retrieves the typed pointer via the get functions below. */
JceAssetHandle jce_asset_acquire(JceAssetManager *mgr,
                                  const char *asset_path,
                                  JceAssetType type);

/* Release an asset (decrement ref count, free at zero). */
void jce_asset_release(JceAssetManager *mgr, JceAssetHandle handle);

/* Get the internal data pointer for a loaded asset.
   Caller must cast to the correct type (JceTexture, JceMesh*, etc.). */
void *jce_asset_get_data(const JceAssetManager *mgr, JceAssetHandle handle);

/* Get the ref count of an asset (0 if invalid). */
uint32_t jce_asset_ref_count(const JceAssetManager *mgr, JceAssetHandle handle);

/* Get the number of loaded assets. */
uint32_t jce_asset_manager_count(const JceAssetManager *mgr);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_MANAGER_H */
