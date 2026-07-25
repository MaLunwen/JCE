/*
 * jce_mesh_lod.h  Automatic LOD (level-of-detail) generation.
 *
 * Pure polygon-reduction core built on meshoptimizer.  Given a triangle
 * mesh (positions + indices) it produces one or more decimated index
 * buffers — the geometric basis of automatic LOD.
 *
 * Public because it is the SAME core the bundle cook bakes into assets:
 * the editor previews the chain it is about to persist, so the numbers it
 * shows must come from this exact implementation rather than a lookalike.
 * Kept out of the internal tree so no consumer has to hand-copy the
 * prototypes and silently drift out of ABI with the engine.
 *
 * No GPU, no scene/ECS — deterministic and side-effect free, so it can run
 * on the bundle worker pool and inside a pure unit test.
 *
 * Determinism: meshopt_simplify / meshopt_simplifySloppy are deterministic
 * for a fixed input, so the generated LOD chain is byte-stable across runs.
 */

#ifndef JCE_MESH_LOD_H
#define JCE_MESH_LOD_H

#include <jce/os/core/jce_defs.h>

#include <stddef.h>

JCE_EXTERN_C_BEGIN

/*
 * Default LOD target ratios used by the bundle converter when no override
 * is supplied.  Monotonic non-increasing fractions of the base triangle
 * count: LOD1 = 50%, LOD2 = 25%, LOD3 = 10%.
 */
#define JCE_MESH_LOD_DEFAULT_LEVEL_COUNT 3u
JCE_API extern const float JCE_MESH_LOD_DEFAULT_RATIOS[JCE_MESH_LOD_DEFAULT_LEVEL_COUNT];

/* Maximum LOD levels a single chain may produce (defensive cap). */
#define JCE_MESH_LOD_MAX_LEVELS 8u

/*
 * Simplify a single triangle mesh to (approximately) target_ratio of its
 * original triangle count.
 *
 *   positions             - interleaved float[3] position stream (xyz).
 *   vertex_count          - number of vertices in `positions`.
 *   position_stride_bytes - byte stride between consecutive positions
 *                           (>= 3*sizeof(float); pass 3*sizeof(float) for
 *                           a tightly packed float3 array).
 *   indices               - base triangle index buffer (index_count entries).
 *   index_count           - number of indices (must be a multiple of 3).
 *   target_ratio          - desired fraction of triangles to KEEP, in
 *                           (0,1].  Values <= 0 or >= 1 short-circuit
 *                           (the input is copied through unchanged).
 *   target_error          - meshopt absolute error budget (e.g. 0.01f);
 *                           larger lets the simplifier collapse more.
 *   out_indices           - caller-provided buffer; MUST have capacity for
 *                           at least `index_count` unsigned ints (the
 *                           simplifier never returns more than the input).
 *
 * Returns the simplified index count (always <= index_count, always a
 * multiple of 3).  All emitted indices are < vertex_count.
 *
 * Behaviour:
 *   - meshopt_simplify preserves topology and may stop early (returning
 *     more triangles than target) to avoid mesh damage.  When the result
 *     is still far above the requested target (more than ~1.5x the target
 *     triangle count) AND an aggressive ratio was requested, we fall back
 *     to meshopt_simplifySloppy, which ignores topology to hit the target.
 *     The sloppy result is used only if it actually reduces further and
 *     stays valid.
 *   - Degenerate-safe: vertex_count == 0, index_count < 3, NULL pointers,
 *     or a non-triangle index_count all copy the input through (or return
 *     0 when there is nothing to copy).  Never crashes, never returns more
 *     than `index_count`.
 */
JCE_API size_t jce_mesh_simplify(const float       *positions,
                                 size_t             vertex_count,
                                 size_t             position_stride_bytes,
                                 const unsigned int *indices,
                                 size_t             index_count,
                                 float              target_ratio,
                                 float              target_error,
                                 unsigned int      *out_indices);

/*
 * Generate a full LOD chain from a single base mesh.
 *
 * Each level is simplified INDEPENDENTLY from the BASE mesh (not chained
 * from the previous LOD), so quality does not compound — this matches how
 * meshopt is intended to be driven for LOD generation.
 *
 *   target_ratios          - array of `level_count` keep-fractions in
 *                            (0,1].  Should be monotonic non-increasing
 *                            (e.g. {0.5, 0.25, 0.1}); the function does not
 *                            re-sort but the produced index counts will be
 *                            non-increasing when the ratios are.
 *   level_count            - number of requested levels (clamped to
 *                            JCE_MESH_LOD_MAX_LEVELS).
 *   out_level_indices      - on success out_level_indices[i] receives a
 *                            freshly allocated index buffer for level i.
 *                            The CALLER owns each buffer and MUST release
 *                            the chain with jce_mesh_lod_chain_free.  On
 *                            allocation failure for any level, ALL
 *                            previously produced levels are freed and 0 is
 *                            returned (no partial ownership handed back).
 *   out_level_index_counts - out_level_index_counts[i] receives the index
 *                            count of level i.
 *
 * Returns the number of levels successfully produced (0 on bad input or
 * allocation failure).  out_level_indices[i] is NULL for any i >= return.
 */
JCE_API size_t jce_mesh_generate_lod_chain(const float       *positions,
                                           size_t             vertex_count,
                                           size_t             position_stride_bytes,
                                           const unsigned int *base_indices,
                                           size_t             base_index_count,
                                           const float       *target_ratios,
                                           size_t             level_count,
                                           unsigned int     **out_level_indices,
                                           size_t            *out_level_index_counts);

/*
 * Free a LOD chain produced by jce_mesh_generate_lod_chain.
 *
 * Frees each non-NULL out_level_indices[i] (for i in [0,level_count)) with
 * the SAME allocator the chain used, and zeroes the pointer.  NULL-safe
 * (NULL array or zero count is a no-op).  Mandatory across the ABI: the
 * engine allocator is not the caller's, so plain free() would be a
 * cross-allocator release.
 */
JCE_API void jce_mesh_lod_chain_free(unsigned int **level_indices, size_t level_count);

JCE_EXTERN_C_END

#endif /* JCE_MESH_LOD_H */
