/*
 * jce_terrain_store.h -- authoritative terrain tile residency (resource-private).
 *
 * WHY THIS EXISTS
 * ---------------
 * Terrain currently has THREE owners: the editor panel holds a JceTerrain*,
 * the renderer loads a second copy into a private cache, and the runtime
 * physics loads a third at spawn.  Saving from the panel invalidates and
 * reloads the whole terrain; the physics copy never observes later edits at
 * all.  Every consumer therefore has its own idea of what the ground is.
 *
 * The store is the single owner.  Consumers acquire a generation-checked
 * handle for a virtual path, pin the tiles they need, read borrowed immutable
 * views, and unpin.  Nobody else loads terrain.
 *
 * WHAT THIS FILE DELIBERATELY IS NOT
 * ----------------------------------
 * Not async yet.  Tiles load through a caller-supplied loader so residency,
 * pinning and eviction can be proven headlessly, without a filesystem or a job
 * system in the test.  Routing that loader through jce_async is a later,
 * separable change that does not alter any contract here.
 *
 * OWNERSHIP RULES
 * ---------------
 *  - A pinned tile is never evicted.  Eviction walks LRU order and skips
 *    anything with a live pin, so a borrowed view can never dangle.
 *  - Handles carry a generation.  A handle retained across release/reacquire
 *    of the same slot fails closed instead of silently resolving to whatever
 *    took the slot -- the defect class that makes stale-handle bugs look like
 *    data corruption.
 *  - Destroy refuses while pins remain, and says so, rather than freeing
 *    memory a consumer is still reading.
 *
 * Layer: Resource (L3) -- PRIVATE.
 */

#ifndef JCE_TERRAIN_STORE_H
#define JCE_TERRAIN_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceTerrainStore JceTerrainStore;

/* index | generation, packed.  0xFFFFFFFF is never a live handle. */
typedef struct { uint32_t bits; } JceTerrainHandle;
#define JCE_TERRAIN_HANDLE_INVALID ((JceTerrainHandle){ 0xFFFFFFFFu })

typedef struct { uint32_t x, z; } JceTerrainTileKey;

/* Borrowed, immutable view of one resident tile.  Valid only until unpin. */
typedef struct {
    JceTerrainTileKey key;
    uint32_t          sample_width;
    uint32_t          sample_height;
    const float      *heights;     /* local-space Y, never normalised */
    uint32_t          _slot;       /* internal: residency slot        */
    uint32_t          _generation; /* internal: validated on unpin    */
} JceTerrainTileView;

/* Loads one tile's heights.  Returns false if the tile does not exist.
 * `out_heights` is owned by the store afterwards and MUST be allocated with
 * the engine allocator (JCE_MALLOC / jce_malloc), because the store frees it
 * with JCE_FREE.  Mixing a raw malloc here with the engine free is a
 * cross-allocator free -- the class of defect this codebase has already had to
 * fix twice.  Keeping this a callback is what lets tests exercise residency
 * without a filesystem. */
typedef bool (*JceTerrainTileLoader)(void *ctx, const char *virtual_path,
                                     JceTerrainTileKey key,
                                     float **out_heights,
                                     uint32_t *out_w, uint32_t *out_h);

typedef struct {
    uint32_t max_assets;         /* distinct terrain paths held at once */
    uint32_t max_resident_tiles; /* residency slots                     */
    uint64_t max_resident_bytes; /* decoded-tile byte budget            */
    JceTerrainTileLoader loader;
    void    *loader_ctx;
} JceTerrainStoreConfig;

/* Low-memory defaults sized for the 512 MB profile.  Zero after init is NOT
 * read as unlimited -- a zero budget would silently disable eviction. */
void jce_terrain_store_config_init(JceTerrainStoreConfig *cfg);

JceTerrainStore *jce_terrain_store_create(const JceTerrainStoreConfig *cfg);

/* Returns false and frees nothing while any tile is still pinned.  The caller
 * unpins and retries; a true return is the only point memory is released. */
bool jce_terrain_store_destroy(JceTerrainStore *store);

/* Acquire by canonical virtual path.  The same path returns the same handle
 * and bumps a refcount, so two consumers share one copy. */
JceTerrainHandle jce_terrain_store_acquire(JceTerrainStore *store,
                                           const char *virtual_path);
void jce_terrain_store_release(JceTerrainStore *store, JceTerrainHandle h);

bool jce_terrain_store_is_valid(const JceTerrainStore *store,
                                JceTerrainHandle h);

/* Pin a tile, loading it on demand.  Fails if the handle is stale, the tile
 * does not exist, or no slot can be freed because everything is pinned. */
bool jce_terrain_store_pin_tile(JceTerrainStore *store, JceTerrainHandle h,
                                JceTerrainTileKey key,
                                JceTerrainTileView *out_view);
void jce_terrain_store_unpin_tile(JceTerrainStore *store,
                                  JceTerrainTileView *view);

/* Diagnostics -- these exist so budgets can be ASSERTED in CI rather than
 * logged and ignored. */
uint32_t jce_terrain_store_resident_tiles(const JceTerrainStore *store);
uint64_t jce_terrain_store_resident_bytes(const JceTerrainStore *store);
uint32_t jce_terrain_store_pinned_tiles(const JceTerrainStore *store);
uint64_t jce_terrain_store_evictions(const JceTerrainStore *store);
uint64_t jce_terrain_store_loads(const JceTerrainStore *store);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TERRAIN_STORE_H */
