/*
 * Scene-owned terrain cache bridge for authoring tools.
 *
 * Kept out of jce_terrain.c because the cook tool links the pure terrain
 * algorithms without the scene container.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_terrain.h>

#include "jce_terrain_cache.h"

typedef struct {
    const char *file_path;
} SceneTerrainFileLoad;

static JceTerrain *scene_terrain_file_load(void *user, const char *asset_path)
{
    const SceneTerrainFileLoad *load = (const SceneTerrainFileLoad *)user;

    (void)asset_path;
    /* jce-terrain-owner-exempt: acquire callback transfers ownership to cache. */
    return jce_terrain_load_file(load->file_path);
}

JceTerrain *JCE_CALL jce_scene_acquire_terrain_file(
    JceScene *scene, const char *asset_path, const char *file_path)
{
    SceneTerrainFileLoad load;
    JceTerrainCache *cache;

    if (!scene || !asset_path || !asset_path[0] ||
        !file_path || !file_path[0])
        return NULL;

    cache = jce_scene_terrain_cache(scene);
    load.file_path = file_path;
    return jce_terrain_cache_acquire(cache, asset_path,
                                     scene_terrain_file_load, &load);
}
