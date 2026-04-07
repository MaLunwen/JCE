/*
 * jce_cgltf_loader.h  glTF model loading via cgltf.
 *
 * Parses glTF/GLB files and converts geometry data into
 * bgfx vertex/index buffers ready for rendering.
 */

#ifndef JCE_CGLTF_LOADER_H
#define JCE_CGLTF_LOADER_H

#include <stdbool.h>
#include <stdint.h>
#include <jce/graphics/jce_gfx_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PakArchive PakArchive;

/* ── Loaded mesh data (CPU side, before GPU upload) ────────────────── */

typedef struct {
    float   *vertices;       /* interleaved: pos(3)+normal(3)+uv(2) */
    uint32_t vertex_count;
    uint16_t *indices;
    uint32_t index_count;
} JceCgltfMeshData;

/* ── Loaded model (collection of meshes) ───────────────────────────── */

typedef struct {
    JceCgltfMeshData *meshes;
    uint32_t          mesh_count;
    char              name[128];
} JceCgltfModel;

/* ── GPU-uploaded model ────────────────────────────────────────────── */

typedef struct {
    uint16_t  vbh_idx;   /* bgfx vertex buffer handle index */
    uint16_t  ibh_idx;   /* bgfx index buffer handle index */
    uint32_t  index_count;
} JceCgltfGpuMesh;

typedef struct {
    JceCgltfGpuMesh *gpu_meshes;
    uint32_t         mesh_count;
} JceCgltfGpuModel;

/* ── API ───────────────────────────────────────────────────────────── */

/* Parse a glTF/GLB file from a memory buffer.
   Returns NULL on failure. Caller must free with jce_cgltf_model_free(). */
JceCgltfModel *jce_cgltf_load_memory(const void *data, size_t size,
                                      const char *name);

/* Load a glTF file from the PAK archive by path. */
JceCgltfModel *jce_cgltf_load_pak(const PakArchive *pak, const char *path);

/* Free a CPU-side model. */
void jce_cgltf_model_free(JceCgltfModel *model);

/* Upload model meshes to bgfx (creates vertex/index buffers).
   Returns a GPU model. Caller must free with jce_cgltf_gpu_model_free(). */
JceCgltfGpuModel *jce_cgltf_upload(const JceCgltfModel *model);

/* Destroy GPU buffers. */
void jce_cgltf_gpu_model_free(JceCgltfGpuModel *gpu);

#ifdef __cplusplus
}
#endif

#endif /* JCE_CGLTF_LOADER_H */
