/*
 * jce_editor_scene_asset_cache.cpp  Shared state + public API.
 */

#include "jce_asset_cache_internal.h"

/* ── State instances (shared via extern in internal header) ──────── */

AssetCacheState  s_cache      = {};
MeshAsyncState   s_mesh_async = {};
TextureAsyncState s_tex_async = {};

/* ── Public API ─────────────────────────────────────────────────── */

void jce_editor_scene_asset_cache_init(JceAssetManager *assets)
{
    if (s_cache.initialized) {
        s_cache.assets = assets;
        return;
    }

    memset(&s_cache, 0, sizeof(s_cache));
    s_cache.assets = assets;
    mesh_async_start();
    texture_async_start();
    s_cache.initialized = true;
}

void jce_editor_scene_asset_cache_shutdown(void)
{
    mesh_async_stop();
    texture_async_stop();
    clear_mesh_cache();
    clear_texture_cache();
    memset(&s_cache, 0, sizeof(s_cache));
}

void jce_editor_scene_asset_cache_finalize(void)
{
    if (!s_cache.initialized)
        return;

    mesh_finalize_completed_loads();
    texture_finalize_completed_loads();
}

void jce_editor_scene_asset_cache_set_scene_dir(const char *dir)
{
    if (!s_cache.initialized)
        return;

    mesh_async_begin_new_generation();
    texture_async_begin_new_generation();

    if (dir) {
        snprintf(s_cache.scene_dir, sizeof(s_cache.scene_dir), "%s", dir);
    } else {
        s_cache.scene_dir[0] = '\0';
    }

    clear_mesh_cache();
    clear_texture_cache();
}

JceMesh *jce_editor_scene_asset_cache_get_mesh(const char *mesh_path,
                                               const float *world_pos)
{
    if (!s_cache.initialized)
        return NULL;
    return asset_cache_get_mesh(mesh_path, world_pos);
}

JceTexture jce_editor_scene_asset_cache_get_texture(const char *material_path,
                                                    const char *mesh_path)
{
    if (!s_cache.initialized)
        return tex_invalid();
    return asset_cache_get_texture(material_path, mesh_path);
}

bool jce_editor_scene_asset_cache_take_texture_warning(const char *material_path,
                                                       const char *mesh_path)
{
    const char *key = (material_path && material_path[0] != '\0')
                    ? material_path : mesh_path;
    if (!key || key[0] == '\0')
        return false;

    int idx = find_texture_cache_entry(key);
    if (idx < 0)
        return false;

    TextureCacheEntry &entry = s_cache.tex_cache[idx];
    if (!entry.failed || entry.warned_missing)
        return false;

    entry.warned_missing = true;
    return true;
}
