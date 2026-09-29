/*
 * jce_terrain_cache.c -- see jce_terrain_cache.h.
 */

#include "jce_terrain_cache.h"

#include <jce/middleware/scene/jce_terrain.h>

#include "os/core/jce_memory.h"
#include <jce/os/core/jce_str.h>

#include <string.h>

#define TERRAIN_CACHE_MAX   16    /* matches the renderer's slot count */
#define TERRAIN_CACHE_PATH  256

struct JceTerrainCache {
    struct {
        bool        used;
        char        path[TERRAIN_CACHE_PATH];
        JceTerrain *terrain;     /* owned */
        uint64_t    revision;    /* >= 1 while resident */
    } slot[TERRAIN_CACHE_MAX];
    /* Global, not per-slot: a slot reused for a different path must not
     * inherit the previous asset's revision, or a consumer that happens to
     * hold the old number would skip the rebuild. */
    uint64_t next_revision;
};

JceTerrainCache *jce_terrain_cache_create(void)
{
    JceTerrainCache *c =
        (JceTerrainCache *)JCE_CALLOC(1, sizeof(JceTerrainCache));
    if (c) c->next_revision = 1u;
    return c;
}

void jce_terrain_cache_destroy(JceTerrainCache *c)
{
    if (!c) return;
    for (int i = 0; i < TERRAIN_CACHE_MAX; i++)
        if (c->slot[i].terrain) jce_terrain_free(c->slot[i].terrain);
    JCE_FREE(c);
}

static int cache_find(const JceTerrainCache *c, const char *path)
{
    for (int i = 0; i < TERRAIN_CACHE_MAX; i++)
        if (c->slot[i].used &&
            strncmp(c->slot[i].path, path, TERRAIN_CACHE_PATH) == 0)
            return i;
    return -1;
}

JceTerrain *jce_terrain_cache_acquire(JceTerrainCache *c, const char *path,
                                      JceTerrainCacheLoadFn load, void *ud)
{
    if (!c || !path || !path[0] || !load) return NULL;

    const int found = cache_find(c, path);
    if (found >= 0)
        /* A resident slot with a NULL terrain IS the remembered failure: the
         * slot being occupied is what stops the retry, so a separate `failed`
         * flag carried no information.  It was removed after a mutation that
         * cleared it changed nothing observable -- dead state that only invites
         * a future divergence between the two representations. */
        return c->slot[found].terrain;

    int slot = -1;
    for (int i = 0; i < TERRAIN_CACHE_MAX; i++)
        if (!c->slot[i].used) { slot = i; break; }
    /* Full.  Refusing beats evicting: every consumer holds a BORROWED pointer,
     * so evicting to make room would free a terrain the renderer is drawing
     * from this frame.  Sixteen distinct terrains in one scene is already far
     * past any real content. */
    if (slot < 0) return NULL;

    JceTerrain *t = load(ud, path);

    c->slot[slot].used = true;
    jce_strlcpy(c->slot[slot].path, path, TERRAIN_CACHE_PATH);
    c->slot[slot].terrain  = t;          /* NULL == remembered failure */
    c->slot[slot].revision = t ? c->next_revision++ : 0u;
    return t;
}

bool jce_terrain_cache_adopt(JceTerrainCache *c, const char *path,
                             JceTerrain *terrain)
{
    if (!c || !path || !path[0] || !terrain) return false;

    int slot = cache_find(c, path);
    if (slot < 0) {
        for (int i = 0; i < TERRAIN_CACHE_MAX; i++) {
            if (!c->slot[i].used) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) return false;

    if (c->slot[slot].terrain && c->slot[slot].terrain != terrain)
        jce_terrain_free(c->slot[slot].terrain);
    c->slot[slot].used = true;
    jce_strlcpy(c->slot[slot].path, path, TERRAIN_CACHE_PATH);
    c->slot[slot].terrain = terrain;
    c->slot[slot].revision = c->next_revision++;
    return true;
}

JceTerrain *jce_terrain_cache_peek(const JceTerrainCache *c, const char *path)
{
    if (!c || !path || !path[0]) return NULL;
    const int i = cache_find(c, path);
    return (i >= 0) ? c->slot[i].terrain : NULL;
}

uint64_t jce_terrain_cache_revision(const JceTerrainCache *c, const char *path)
{
    if (!c || !path || !path[0]) return 0u;
    const int i = cache_find(c, path);
    return (i >= 0) ? c->slot[i].revision : 0u;
}

bool jce_terrain_cache_touch(JceTerrainCache *c, const char *path)
{
    if (!c || !path || !path[0]) return false;
    const int i = cache_find(c, path);
    if (i < 0 || !c->slot[i].terrain) return false;
    c->slot[i].revision = c->next_revision++;
    return true;
}

static void cache_drop(JceTerrainCache *c, int i)
{
    if (c->slot[i].terrain) jce_terrain_free(c->slot[i].terrain);
    memset(&c->slot[i], 0, sizeof(c->slot[i]));
}

void jce_terrain_cache_invalidate(JceTerrainCache *c, const char *path)
{
    if (!c) return;
    if (!path || !path[0]) {
        for (int i = 0; i < TERRAIN_CACHE_MAX; i++)
            if (c->slot[i].used) cache_drop(c, i);
        return;
    }
    const int i = cache_find(c, path);
    if (i >= 0) cache_drop(c, i);
}

uint32_t jce_terrain_cache_resident(const JceTerrainCache *c)
{
    if (!c) return 0u;
    uint32_t n = 0u;
    for (int i = 0; i < TERRAIN_CACHE_MAX; i++)
        if (c->slot[i].used && c->slot[i].terrain) n++;
    return n;
}
