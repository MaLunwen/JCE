/*
 * jce_model_loader_assimp.h  Assimp model loading helpers (editor-only).
 */

#ifndef JCE_MODEL_LOADER_ASSIMP_H
#define JCE_MODEL_LOADER_ASSIMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/graphics/jce_mesh.h>
#include <jce/core/pak_loader.h>

typedef struct JceEditorCpuMeshData {
    JceMeshVertex *vertices;
    uint32_t       vertex_count;
    uint32_t      *indices;
    uint32_t       index_count;
} JceEditorCpuMeshData;

JceMesh *jce_editor_model_load(const PakArchive *pak, const char *asset_path);
JceMesh *jce_editor_model_load_file(const char *file_path);

bool jce_editor_model_load_cpu_file(const char *file_path,
                                    JceEditorCpuMeshData *out);
void jce_editor_model_free_cpu_data(JceEditorCpuMeshData *data);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_LOADER_ASSIMP_H */
