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

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Slot layout                                                         */
/* ================================================================== */

typedef struct JceAssetSlot {
	uint64_t      path_hash;      /* XXH3_64 of asset path */
	char         *path;           /* owned copy of source asset path */
	JceAssetType  type;
	JceAssetState state;
	uint16_t      generation;     /* generation counter for this slot */
	uint32_t      ref_count;
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
};

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_MANAGER_NEW_H */
