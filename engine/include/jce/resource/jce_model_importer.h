/*
 * jce_model_importer.h  Model file import (assimp-backed) — public API.
 *
 * Two API levels:
 *   1. Loaders        Convert a model file/PAK asset into a runtime JceMesh
 *                       or a CPU-side mesh-data buffer.
 *   2. Inspector      Lightweight metadata extraction (counts, names,
 *                       optional flat vertex/face arrays) for previewers.
 *
 * Supported formats: anything assimp groks — OBJ, FBX, glTF, GLB, 3DS, ...
 */

#ifndef JCE_MODEL_IMPORTER_H
#define JCE_MODEL_IMPORTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_pak_loader.h>
#include <jce/renderer/jce_mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── CPU-side decoded mesh ───────────────────────────────────── */
typedef struct JceModelCpuMeshData {
    JceMeshVertex *vertices;
    uint32_t       vertex_count;
    uint32_t      *indices;
    uint32_t       index_count;
} JceModelCpuMeshData;

/* ─── Material extraction result ──────────────────────────────── */
typedef struct JceModelMaterialInfo {
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
} JceModelMaterialInfo;

/* ─── Loaders ─────────────────────────────────────────────────── */
JCE_API JceMesh *jce_model_importer_load_pak(const JcePakArchive *pak,
                                             const char          *asset_path);
JCE_API JceMesh *jce_model_importer_load_file(const char *file_path);

JCE_API bool jce_model_importer_load_cpu_file(const char          *file_path,
                                              JceModelCpuMeshData *out);
JCE_API void jce_model_importer_free_cpu(JceModelCpuMeshData *data);

JCE_API bool jce_model_importer_extract_material(const char           *file_path,
                                                 JceModelMaterialInfo *out);

/* ─── Inspector (preview-time metadata) ───────────────────────── */
typedef struct JceModelInspectResult {
    int   mesh_count;
    int   material_count;
    int   vertex_count;
    int   face_count;

    float bounds_min[3];
    float bounds_max[3];

    /* Optional flat arrays (only populated when want_wireframe=true).
     * Allocated with jce_malloc — release via jce_model_importer_free_inspect. */
    float    *vertices_xyz;       /* 3 * vertex_count */
    int      *face_indices;       /* concat of face indices */
    int      *face_sizes;         /* one entry per face */
    int       face_indices_count;

    /* Material name table (always filled).  Each entry NUL-terminated;
     * outer array has material_count entries.  Released by
     * jce_model_importer_free_inspect. */
    char    **material_names;

    char      error[256];
} JceModelInspectResult;

JCE_API bool jce_model_importer_inspect_memory(const void *data, size_t size,
                                               const char *ext_hint,
                                               bool        want_wireframe,
                                               JceModelInspectResult *out);
JCE_API bool jce_model_importer_inspect_file(const char *file_path,
                                             bool        want_wireframe,
                                             JceModelInspectResult *out);
JCE_API void jce_model_importer_free_inspect(JceModelInspectResult *r);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MODEL_IMPORTER_H */
