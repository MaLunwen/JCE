/*
 * jce_proc_mesh.h  Runtime procedural mesh authoring.
 *
 * Unity-style Mesh.SetVertices / SetTriangles / SetUVs equivalents.
 * Builds CPU-side vertex + index arrays that can later be uploaded
 * via the renderer's standard mesh path (jce_mesh).  The upload
 * mechanic is wired in B17; this module owns the typed buffer model.
 *
 * Workflow
 *   JceProcMesh m;
 *   jce_proc_mesh_init(&m);
 *   jce_proc_mesh_set_vertices(&m, verts, count);
 *   jce_proc_mesh_set_triangles(&m, tris, tri_count);
 *   // optionally:
 *   jce_proc_mesh_set_uvs(&m, uvs, uv_count);
 *   jce_proc_mesh_set_normals(&m, normals, count);
 *   // when ready, upload via jce_mesh:
 *   // jce_proc_mesh_to_mesh(&m, &mesh_out);
 *   jce_proc_mesh_dispose(&m);
 *
 * Dirty flags let the upload path skip channels that haven't changed
 * between rebuilds (e.g. animating only UVs).
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_PROC_MESH_H
#define JCE_PROC_MESH_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_PROC_MESH_DIRTY_VERTICES = 1u << 0,
    JCE_PROC_MESH_DIRTY_NORMALS  = 1u << 1,
    JCE_PROC_MESH_DIRTY_UVS      = 1u << 2,
    JCE_PROC_MESH_DIRTY_COLORS   = 1u << 3,
    JCE_PROC_MESH_DIRTY_INDICES  = 1u << 4,
    JCE_PROC_MESH_DIRTY_ALL      = 0x1Fu,
} JceProcMeshDirty;

typedef enum {
    JCE_PROC_MESH_TOPO_TRIANGLES = 0,
    JCE_PROC_MESH_TOPO_LINES     = 1,
    JCE_PROC_MESH_TOPO_POINTS    = 2,
} JceProcMeshTopology;

typedef struct {
    /* Vertex channels — each allocated lazily.  Sizes refer to
     * element count (not byte count). */
    float    *positions;     /* xyz, 3 floats per vertex */
    float    *normals;       /* xyz, 3 floats per vertex */
    float    *uvs;           /* uv, 2 floats per vertex */
    uint32_t *colors;        /* RGBA8 packed */
    uint32_t  vertex_count;
    uint32_t  vertex_capacity;

    /* Indices — uint32 to support >65k meshes; downgrade to uint16
     * happens at upload if max-index permits. */
    uint32_t *indices;
    uint32_t  index_count;
    uint32_t  index_capacity;

    JceProcMeshTopology topology;

    /* Bit-OR of JCE_PROC_MESH_DIRTY_*.  Cleared by callers after
     * uploading the corresponding channel to GPU. */
    uint32_t  dirty_flags;

    /* Optional name (debug / asset path).  Not used for hashing. */
    char      name[64];
} JceProcMesh;

/* ── Lifecycle ───────────────────────────────────────────────── */

JCE_API void jce_proc_mesh_init   (JceProcMesh *m);
JCE_API void jce_proc_mesh_dispose(JceProcMesh *m);
JCE_API void jce_proc_mesh_clear  (JceProcMesh *m);

/* ── Channel setters (copy semantics) ─────────────────────────── */

JCE_API bool jce_proc_mesh_set_vertices (JceProcMesh *m,
                                          const float *xyz_triples,
                                          uint32_t      vertex_count);
JCE_API bool jce_proc_mesh_set_normals  (JceProcMesh *m,
                                          const float *xyz_triples,
                                          uint32_t      vertex_count);
JCE_API bool jce_proc_mesh_set_uvs      (JceProcMesh *m,
                                          const float *uv_pairs,
                                          uint32_t      vertex_count);
JCE_API bool jce_proc_mesh_set_colors   (JceProcMesh *m,
                                          const uint32_t *rgba,
                                          uint32_t        vertex_count);
JCE_API bool jce_proc_mesh_set_triangles(JceProcMesh *m,
                                          const uint32_t *indices,
                                          uint32_t        index_count);

JCE_API void jce_proc_mesh_set_topology (JceProcMesh *m,
                                          JceProcMeshTopology t);

/* Mark every channel dirty (force re-upload on next sync). */
JCE_API void jce_proc_mesh_mark_all_dirty(JceProcMesh *m);

/* ── Convenience builders ─────────────────────────────────────── */

/* Recompute per-vertex normals from positions + triangle indices.
 * Overwrites the normal channel and marks it dirty. */
JCE_API bool jce_proc_mesh_recalc_normals(JceProcMesh *m);

/* Append a quad (two triangles) — useful for procedurally-stitched
 * meshes.  Vertices are appended in CCW order.  Each `p*` is xyz. */
JCE_API bool jce_proc_mesh_append_quad(JceProcMesh *m,
                                        const float *p0, const float *p1,
                                        const float *p2, const float *p3);

JCE_EXTERN_C_END

#endif /* JCE_PROC_MESH_H */
