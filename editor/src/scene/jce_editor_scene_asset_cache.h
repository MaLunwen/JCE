/*
 * jce_editor_scene_asset_cache.h  Editor scene asset cache helpers.
 *
 * Owns async mesh/texture loading, scene-relative path resolution,
 * and cache lifecycle for the editor scene viewport.
 */

#ifndef JCE_EDITOR_SCENE_ASSET_CACHE_H
#define JCE_EDITOR_SCENE_ASSET_CACHE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/resource/jce_asset.h>

void jce_editor_scene_asset_cache_init(JceAssetManager *assets);
void jce_editor_scene_asset_cache_shutdown(void);
void jce_editor_scene_asset_cache_finalize(void);
void jce_editor_scene_asset_cache_set_scene_dir(const char *dir);

JceMesh *jce_editor_scene_asset_cache_get_mesh(const char *mesh_path,
                                               const float *world_pos);
JceTexture jce_editor_scene_asset_cache_get_texture(const char *material_path,
                                                    const char *mesh_path);

/* Returns true exactly once per failed material/mesh pair so callers can log once. */
bool jce_editor_scene_asset_cache_take_texture_warning(const char *material_path,
                                                       const char *mesh_path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_SCENE_ASSET_CACHE_H */