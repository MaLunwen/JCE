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
#include <jce/resource/jce_pak_loader.h>
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
    char  albedo_tex[256];
    char  mr_tex[256];
    char  normal_tex[256];
    char  ao_tex[256];
    char  emissive_tex[256];
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

/* ─── Skinned / animated model import (FBX, DAE, …) ────────────────
 *
 * The loaders above flatten the node hierarchy (aiProcess_PreTransformVertices)
 * and emit a STATIC mesh only — they discard bones, skin weights, and
 * animations.  These entry points instead extract a full skeletal model
 * (skeleton + skinned vertices + animation clips) into the SAME JceModel the
 * glTF runtime loader produces, so the renderer / animation system consume it
 * unchanged.  Detection is automatic: a scene with no bones falls back to a
 * static single-node model, so a non-skinned FBX/OBJ still loads.
 *
 * Two API levels mirror the glTF worker/render-thread split:
 *   - _decode_skinned_* run the assimp parse + CPU extraction (no GPU / bgfx) and
 *     return an opaque JceModelCpu intermediate.  Safe on a worker thread (or in
 *     a headless unit test).  Inspect it via the engine-internal jce_gltf_cpu_*
 *     accessors, then upload it on the render thread with jce_model_upload_gltf_cpu
 *     (declared in <jce/renderer/jce_model.h>), which CONSUMES it.  Release an
 *     un-uploaded result with jce_model_gltf_cpu_free.
 *   - _load_skinned_* are sync convenience wrappers that decode + upload in one
 *     call and return a ready JceModel*; they MUST be called on the render thread
 *     (they create bgfx GPU buffers).  Return NULL on failure.
 *
 * The existing static loaders are unchanged. */
/* These opaque types are also forward-declared by <jce/renderer/jce_model.h>.
 * Independent guards (matching that header's) so a TU including both does not
 * hit a duplicate-typedef diagnostic under strict C99. */
#ifndef JCE_MODEL_FWD_DECLARED
#define JCE_MODEL_FWD_DECLARED
typedef struct JceModel    JceModel;
#endif
#ifndef JCE_MODELCPU_FWD_DECLARED
#define JCE_MODELCPU_FWD_DECLARED
typedef struct JceModelCpu JceModelCpu;
#endif

JCE_API JceModelCpu *jce_model_importer_decode_skinned_pak(const JcePakArchive *pak,
                                                           const char *asset_path);
JCE_API JceModelCpu *jce_model_importer_decode_skinned_file(const char *file_path);

JCE_API JceModel *jce_model_importer_load_skinned_pak(const JcePakArchive *pak,
                                                      const char *asset_path);
JCE_API JceModel *jce_model_importer_load_skinned_file(const char *file_path);

/* ─── Per-part extraction (collider cooking) ──────────────────────
 * Unlike the loaders above, this does NOT flatten the node hierarchy.
 * Each scene node that carries geometry becomes one part, named by the
 * node, with its meshes merged and its accumulated world transform kept
 * separate (column-major). This preserves the "N separated objects"
 * structure a model file expresses so colliders stay per-object instead
 * of collapsing into one fat hull spanning the empty gaps between them.
 *
 * Geometry is part-local; apply `transform` to reach model space. The
 * layout matches JceColliderPart so a part feeds the cooker directly. */
typedef struct JceModelPart {
    char      name[128];
    float    *positions;     /* 3 * vertex_count, part-local            */
    uint32_t  vertex_count;
    uint32_t *indices;       /* 3 per triangle, local to this part      */
    uint32_t  index_count;
    float     transform[16]; /* part-local → model space, column-major  */
} JceModelPart;

typedef struct JceModelParts {
    JceModelPart *parts;
    uint32_t      count;
} JceModelParts;

JCE_API bool jce_model_importer_load_parts_file(const char     *file_path,
                                                JceModelParts  *out);
JCE_API bool jce_model_importer_load_parts_memory(const void    *data,
                                                  size_t          size,
                                                  const char     *ext_hint,
                                                  JceModelParts  *out);
JCE_API void jce_model_importer_free_parts(JceModelParts *parts);

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
