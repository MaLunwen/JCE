/*
 * jce_model_loader_assimp.h  Assimp model loading helpers (editor-only).
 */

#ifndef JCE_MODEL_LOADER_ASSIMP_H
#define JCE_MODEL_LOADER_ASSIMP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/renderer/jce_mesh.h>
#include <jce/os/core/pak_loader.h>

typedef struct JceEditorCpuMeshData {
    JceMeshVertex *vertices;
    uint32_t       vertex_count;
    uint32_t      *indices;
    uint32_t       index_count;
} JceEditorCpuMeshData;

typedef struct JceEditorMaterialInfo {
    char  albedo_tex[128];
    char  mr_tex[128];
    char  normal_tex[128];
    char  ao_tex[128];
    char  emissive_tex[128];
    float base_color[4];
    float metallic;
    float roughness;
    float emissive[3];
    float normal_scale;
    float ao_strength;
    int   alpha_mode;       /* 0=OPAQUE, 1=MASK, 2=BLEND */
    float alpha_cutoff;
    bool  double_sided;
} JceEditorMaterialInfo;

JceMesh *jce_editor_model_load(const JcePakArchive *pak, const char *asset_path);
JceMesh *jce_editor_model_load_file(const char *file_path);

bool jce_editor_model_load_cpu_file(const char *file_path,
                                    JceEditorCpuMeshData *out);
void jce_editor_model_free_cpu_data(JceEditorCpuMeshData *data);

bool jce_editor_model_extract_material(const char *file_path,
                                       JceEditorMaterialInfo *out);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_LOADER_ASSIMP_H */
