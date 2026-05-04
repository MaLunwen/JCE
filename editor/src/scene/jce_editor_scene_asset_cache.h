/*
 * jce_editor_scene_asset_cache.h  Editor scene asset cache helpers.
 *
 * Owns async mesh/texture loading, scene-relative path resolution,
 * and cache lifecycle for the editor scene viewport.
 */

#ifndef JCE_EDITOR_SCENE_ASSET_CACHE_H
#define JCE_EDITOR_SCENE_ASSET_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_asset.h>

#include "jce_model_loader_assimp.h"

typedef struct JceEditorMaterialExtractResult {
    uint32_t              entity_id;
    char                  mesh_path[256];
    bool                  success;
    JceEditorMaterialInfo material;
} JceEditorMaterialExtractResult;

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

/* Returns true ONLY if a texture lookup for (material_path, mesh_path)
 * has been attempted and is known to have permanently failed. Returns
 * false when the texture is loaded, still being resolved/loaded, or has
 * not yet been requested. Used by the engine renderer to suppress the
 * magenta/yellow "missing texture" checker during the in-flight window
 * after a scene loads, so textures don't flash white→pink-checker→final
 * as each async load completes. */
bool jce_editor_scene_asset_cache_texture_failed(const char *material_path,
                                                 const char *mesh_path);

void jce_editor_scene_asset_cache_queue_material_extract(uint32_t entity_id,
                                                         const char *mesh_path,
                                                         const char *file_path);
bool jce_editor_scene_asset_cache_take_material_result(
    JceEditorMaterialExtractResult *out_result);

/* Resolve a possibly-relative material/mesh path to an existing absolute
 * path on disk using all known asset roots (scene dir, scene parent,
 * project root, asset-browser project root) and a fuzzy basename search.
 * Returns true and writes the resolved path into out_buf on success.
 */
bool jce_editor_scene_asset_cache_resolve_material_path(const char *material_path,
                                                       char *out_buf, int out_size);
bool jce_editor_scene_asset_cache_resolve_mesh_path(const char *mesh_path,
                                                   char *out_buf, int out_size);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_SCENE_ASSET_CACHE_H */