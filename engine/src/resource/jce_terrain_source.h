/*
 * jce_terrain_source.h -- v3-file-backed tile loader for the terrain store.
 *
 * The store deliberately takes a loader callback rather than reaching for the
 * filesystem itself, so residency, pinning and eviction stay testable with no
 * I/O.  This is the production loader that plugs the store into the cooked v3
 * format over the JCE virtual filesystem.
 *
 * WHY IT HOLDS THE FILE OPEN
 * --------------------------
 * The whole reason v3 has a directory and per-tile payloads is random access:
 * a 4097x4097 terrain must not be read whole to page in one tile.  So the
 * source parses the header and directory ONCE at open, keeps them, and reads
 * only the requested tile's byte range afterwards.  Re-reading and re-parsing
 * the file per tile would give back exactly the property the format exists to
 * provide.
 *
 * The current implementation keeps the file bytes resident and slices them.
 * Swapping that for a seek+range read is a contained change behind this same
 * interface -- the store never learns which it is.
 *
 * Layer: Resource (L3) -- PRIVATE.
 */

#ifndef JCE_TERRAIN_SOURCE_H
#define JCE_TERRAIN_SOURCE_H

#include "jce_terrain_store.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceTerrainSource JceTerrainSource;

struct JceFileSystem;

/* Open a cooked v3 terrain.  Verifies the header and the directory up front,
 * so a corrupt file fails HERE rather than at the first tile pin -- a caller
 * that got a handle should not later discover the asset was never readable.
 * Returns NULL on any validation failure. */
JceTerrainSource *jce_terrain_source_open(const struct JceFileSystem *fs,
                                          const char *virtual_path);

/* Open from a caller-owned memory image.  The buffer must outlive the source.
 * Exists so the codec+store integration is testable without a filesystem. */
JceTerrainSource *jce_terrain_source_open_memory(const void *data,
                                                 size_t size);

void jce_terrain_source_close(JceTerrainSource *src);

/* Dimensions, for callers that need to size a query before pinning anything. */
uint32_t jce_terrain_source_tiles_x(const JceTerrainSource *src);
uint32_t jce_terrain_source_tiles_z(const JceTerrainSource *src);
float    jce_terrain_source_base_height(const JceTerrainSource *src);
float    jce_terrain_source_height_range(const JceTerrainSource *src);
uint8_t  jce_terrain_source_diagonal_rule(const JceTerrainSource *src);

/* JceTerrainTileLoader-compatible.  `ctx` must be a JceTerrainSource*.
 *
 * Note the store owns the returned buffer and frees it with JCE_FREE, so this
 * allocates with the engine allocator -- mixing those is the cross-allocator
 * free this codebase has already had to fix twice. */
bool jce_terrain_source_load_tile(void *ctx, const char *virtual_path,
                                  JceTerrainTileKey key,
                                  float **out_heights,
                                  uint32_t *out_w, uint32_t *out_h);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TERRAIN_SOURCE_H */
