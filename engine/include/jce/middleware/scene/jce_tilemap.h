/*
 * jce_tilemap.h  Tilemap asset loader + renderer (stub for P5).
 */

#ifndef JCE_TILEMAP_H
#define JCE_TILEMAP_H

#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceTilemapAsset JceTilemapAsset;

JCE_API JceTilemapAsset *jce_tilemap_load(const char *path);
JCE_API void             jce_tilemap_unload(JceTilemapAsset *t);

JCE_API uint32_t         jce_tilemap_width (const JceTilemapAsset *t);
JCE_API uint32_t         jce_tilemap_height(const JceTilemapAsset *t);
JCE_API uint32_t         jce_tilemap_tile_at(const JceTilemapAsset *t, uint32_t x, uint32_t y);

JCE_EXTERN_C_END

#endif /* JCE_TILEMAP_H */
