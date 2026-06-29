/*
 * jce_hlod_bake.h  Engine HLOD proxy bake (Direction A2).
 *
 * Given a cell's source meshes (CPU positions/normals/indices + each entity's
 * world matrix), merges them into world space, simplifies the result with
 * meshopt (jce_mesh_simplify), and writes a single-mesh proxy .glb
 * (jce_glb_write) the streaming HLOD layer shows when the cell is unloaded.
 *
 * Pure transform + I/O: the caller (editor) supplies CPU geometry + matrices;
 * this does the merge/simplify/write.  No bgfx, no scene access.
 */

#ifndef JCE_HLOD_BAKE_H
#define JCE_HLOD_BAKE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* One source mesh instance to fold into the cell proxy.  Positions/normals may
 * be strided (e.g. point straight at an interleaved JcePbrVertex array via
 * offsetof) so the caller need not de-interleave. */
typedef struct {
    const void     *positions;     /* ptr to vertex 0's float[3] position       */
    uint32_t        position_stride;/* bytes between vertices (0 → 12, tight)    */
    const void     *normals;       /* ptr to vertex 0's float[3] normal; NULL→+Y */
    uint32_t        normal_stride; /* bytes between vertices (0 → 12, tight)     */
    uint32_t        vertex_count;
    const uint32_t *indices;       /* triangle list (multiple of 3)            */
    uint32_t        index_count;
    float           world[16];     /* column-major world matrix for this mesh   */
} JceHlodMeshInput;

typedef struct {
    uint32_t in_vertices, in_triangles;
    uint32_t out_vertices, out_triangles;
} JceHlodBakeStats;

/* Merge `inputs` into world space, simplify to ~`target_ratio` of the triangles
 * (clamped (0,1]; e.g. 0.15 keeps 15%), and write a proxy .glb to
 * `out_glb_host_path` tinted `base_color` (RGBA).  The proxy is authored in WORLD
 * space, so the HLOD proxy entity sits at the origin (matches build/gen_hlod.py +
 * jce_world_streamer_attach_hlod).  Returns false on bad args / empty input /
 * write failure.  `out_stats` may be NULL. */
JCE_API bool jce_hlod_bake_proxy(const JceHlodMeshInput *inputs,
                                 uint32_t                input_count,
                                 float                   target_ratio,
                                 const float             base_color[4],
                                 const char             *out_glb_host_path,
                                 JceHlodBakeStats       *out_stats);

JCE_EXTERN_C_END

#endif /* JCE_HLOD_BAKE_H */
