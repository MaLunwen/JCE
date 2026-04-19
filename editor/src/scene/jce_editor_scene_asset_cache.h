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

#include <jce/graphics/jce_texture.h>
#include <jce/graphics/jce_mesh.h>
#include <jce/resource/jce_asset.h>
#include "jce_model_loader_assimp.h"

typedef struct JceEditorMaterialExtractResult {
    uint32_t              entity_id;
    char                  mesh_path[128];
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

void jce_editor_scene_asset_cache_queue_material_extract(uint32_t entity_id,
                                                         const char *mesh_path,
                                                         const char *file_path);
bool jce_editor_scene_asset_cache_take_material_result(
    JceEditorMaterialExtractResult *out_result);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_SCENE_ASSET_CACHE_H */