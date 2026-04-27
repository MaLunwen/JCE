/*
 * jce_model_loader_assimp.h  Editor-side compatibility alias for the
 * engine's public model importer (jce/resource/jce_model_importer.h).
 *
 * Kept so existing editor call sites keep building during the assimp
 * wrap-up; new code should include the engine header directly.
 */

#ifndef JCE_MODEL_LOADER_ASSIMP_H
#define JCE_MODEL_LOADER_ASSIMP_H

#include <jce/resource/jce_model_importer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef JceModelCpuMeshData  JceEditorCpuMeshData;
typedef JceModelMaterialInfo JceEditorMaterialInfo;

static inline JceMesh *jce_editor_model_load(const JcePakArchive *pak,
                                             const char          *asset_path)
{
    return jce_model_importer_load_pak(pak, asset_path);
}

static inline JceMesh *jce_editor_model_load_file(const char *file_path)
{
    return jce_model_importer_load_file(file_path);
}

static inline bool jce_editor_model_load_cpu_file(const char           *file_path,
                                                  JceEditorCpuMeshData *out)
{
    return jce_model_importer_load_cpu_file(file_path, out);
}

static inline void jce_editor_model_free_cpu_data(JceEditorCpuMeshData *data)
{
    jce_model_importer_free_cpu(data);
}

static inline bool jce_editor_model_extract_material(const char            *file_path,
                                                     JceEditorMaterialInfo *out)
{
    return jce_model_importer_extract_material(file_path, out);
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_LOADER_ASSIMP_H */
