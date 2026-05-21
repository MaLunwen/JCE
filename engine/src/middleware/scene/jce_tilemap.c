/*
 * jce_tilemap.c  Stub tilemap asset; real loader lands in P5.
 */

#include <jce/middleware/scene/jce_tilemap.h>
#include "os/core/jce_memory.h"
#include <stddef.h>

struct JceTilemapAsset {
    uint32_t  width;
    uint32_t  height;
    uint32_t *cells;
};

JceTilemapAsset *jce_tilemap_load(const char *path)
{
    (void)path;
    JceTilemapAsset *t = (JceTilemapAsset *)JCE_CALLOC(1, sizeof(JceTilemapAsset));
    return t;
}

void jce_tilemap_unload(JceTilemapAsset *t)
{
    if (!t) return;
    if (t->cells) JCE_FREE(t->cells);
    JCE_FREE(t);
}

uint32_t jce_tilemap_width(const JceTilemapAsset *t)
{
    return t ? t->width : 0;
}

uint32_t jce_tilemap_height(const JceTilemapAsset *t)
{
    return t ? t->height : 0;
}

uint32_t jce_tilemap_tile_at(const JceTilemapAsset *t, uint32_t x, uint32_t y)
{
    if (!t || !t->cells || x >= t->width || y >= t->height) return 0;
    return t->cells[y * t->width + x];
}
