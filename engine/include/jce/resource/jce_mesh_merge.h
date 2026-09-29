/*
 * jce_mesh_merge.h  Merge many placed meshes into one, in world space.
 *
 * THE ONE MERGE.  Two features need it and neither may own it:
 *
 *   HLOD proxy bake (jce_hlod_bake.h) merges a streaming cell's meshes and then
 *   SIMPLIFIES the result to a distant stand-in -- lossy on purpose.
 *
 *   STATIC BATCHING (jce_static_batch.h) merges meshes that share a material
 *   and keeps every triangle and every UV -- lossless on purpose, because the
 *   result replaces the originals at full detail.
 *
 * They differ in what happens AFTER the merge, not in the merge.  It lived
 * inside the HLOD bake, so the second caller would have had to copy the
 * transform-into-world loop -- and a merge that transforms normals slightly
 * differently from the merge next to it is a lighting difference nobody would
 * trace back to a copied loop.
 *
 * Pure transform: CPU in, CPU out.  No bgfx, no scene access, no I/O.
 */

#ifndef JCE_MESH_MERGE_H
#define JCE_MESH_MERGE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* One placed mesh to fold in.  Attributes may be STRIDED, so a caller can point
 * straight at an interleaved vertex array (e.g. via offsetof on JceMeshVertex)
 * instead of de-interleaving it first. */
typedef struct {
    const void     *positions;      /* -> vertex 0's float[3] position        */
    uint32_t        position_stride;/* bytes between vertices (0 -> 12)       */
    const void     *normals;        /* -> vertex 0's float[3]; NULL -> +Y     */
    uint32_t        normal_stride;  /* bytes between vertices (0 -> 12)       */
    const void     *uvs;            /* -> vertex 0's float[2]; NULL -> (0,0)  */
    uint32_t        uv_stride;      /* bytes between vertices (0 -> 8)        */
    uint32_t        vertex_count;
    const uint32_t *indices;        /* triangle list (multiple of 3)          */
    uint32_t        index_count;
    float           world[16];      /* column-major world matrix              */
} JceMeshMergeInput;

/* The merged result.  Tightly packed, engine-allocated; free with
 * jce_mesh_merge_free.  `uvs` is NULL when NO input carried any -- an all-zero
 * UV set and no UV set are different things to the .glb writer, and a merge of
 * untextured meshes should not grow a texture coordinate. */
typedef struct {
    float    *positions;      /* float[3] * vertex_count */
    float    *normals;        /* float[3] * vertex_count */
    float    *uvs;            /* float[2] * vertex_count, or NULL */
    uint32_t  vertex_count;
    uint32_t *indices;        /* triangle list */
    uint32_t  index_count;
} JceMeshMergeResult;

/* Transform every input into world space and concatenate.  Positions go through
 * the full matrix; normals through its upper 3x3 and are renormalised, which is
 * why a mirrored or non-uniformly scaled instance still lights correctly.  UVs
 * are copied verbatim.
 *
 * Inputs with no positions or no indices are SKIPPED, not refused: a caller
 * assembling a group from a scene will meet an entity whose mesh failed to
 * load, and losing the whole batch to it would be worse than losing the one.
 *
 * Returns false on bad arguments, empty input, or allocation failure; `out` is
 * zeroed on failure so jce_mesh_merge_free is always safe to call. */
JCE_API bool jce_mesh_merge(const JceMeshMergeInput *inputs,
                            uint32_t                 input_count,
                            JceMeshMergeResult      *out);

/* Where one INPUT ended up inside the merged buffers.
 *
 * WHY THE MERGE HAS TO REPORT THIS.  A merged group is one draw, which is the
 * whole point, and it is also ONE CULLABLE OBJECT -- so a group whose far end
 * is behind the camera still draws every triangle it has.  Unity's Static
 * Batching does not pay that: it keeps each renderer's index sub-range into
 * the shared buffer and submits only the ranges it can see.  The sub-ranges
 * are a property of the CONCATENATION and are known only here, at the moment
 * the inputs are laid end to end; a consumer that tried to recover them later
 * would be re-deriving something this loop already knew.
 *
 * `merged` is false for an input the merge SKIPPED (no positions, no indices
 * -- see the note on jce_mesh_merge).  Its spans are zero, and a caller must
 * not treat index 0, count 0 as "the first triangle": the array is indexed by
 * INPUT, so the skipped entries have to stay in place or every span after
 * them would describe the wrong mesh. */
typedef struct {
    uint32_t first_index;    /* into JceMeshMergeResult::indices  */
    uint32_t index_count;
    uint32_t first_vertex;   /* into ::positions / ::normals / ::uvs */
    uint32_t vertex_count;
    float    aabb_min[3];    /* WORLD space, from the transformed vertices */
    float    aabb_max[3];
    bool     merged;
} JceMeshMergeSpan;

/* jce_mesh_merge, and it also tells you where each input landed.
 *
 * `out_spans` has `input_count` entries, one per INPUT in the order given.
 * The world-space AABB comes from the vertices this merge actually wrote, not
 * from a local bound pushed through the matrix, so a mirrored or sheared
 * instance gets the box its geometry really occupies.
 *
 * jce_mesh_merge is this with out_spans = NULL and is byte-identical to what
 * it always was. */
JCE_API bool jce_mesh_merge_spans(const JceMeshMergeInput *inputs,
                                  uint32_t                 input_count,
                                  JceMeshMergeResult      *out,
                                  JceMeshMergeSpan        *out_spans);

/* Free what jce_mesh_merge allocated and zero the struct.  NULL-safe. */
JCE_API void jce_mesh_merge_free(JceMeshMergeResult *r);

JCE_EXTERN_C_END

#endif /* JCE_MESH_MERGE_H */
