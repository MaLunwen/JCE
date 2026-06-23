/*
 * jce_mesh_lod_cook.c  Bundle-time automatic LOD generation (INTERNAL).
 *
 * Implementation of the meshoptimizer-backed polygon-reduction core
 * declared in jce_mesh_lod_cook.h.  Pure C99; depends only on
 * meshoptimizer, the engine allocator, and the C standard library.
 */

#include "jce_mesh_lod_cook.h"

#include "os/core/jce_memory.h"

#include <meshoptimizer.h>

#include <string.h>

/* LOD1=50%, LOD2=25%, LOD3=10% of base triangle count. */
const float JCE_MESH_LOD_DEFAULT_RATIOS[JCE_MESH_LOD_DEFAULT_LEVEL_COUNT] = {
    0.5f, 0.25f, 0.1f
};

/* Copy the base index buffer through verbatim (degenerate / passthrough). */
static size_t lod_passthrough(const unsigned int *indices,
                              size_t              index_count,
                              unsigned int       *out_indices)
{
    if (!out_indices) return 0;
    if (!indices || index_count == 0) return 0;
    memcpy(out_indices, indices, index_count * sizeof(unsigned int));
    return index_count;
}

size_t jce_mesh_simplify(const float        *positions,
                         size_t              vertex_count,
                         size_t              position_stride_bytes,
                         const unsigned int *indices,
                         size_t              index_count,
                         float               target_ratio,
                         float               target_error,
                         unsigned int       *out_indices)
{
    /* ---- degenerate / passthrough guards -------------------------------- */
    if (!out_indices) return 0;

    /* Nothing meaningful to simplify: fewer than one triangle, no verts, no
     * position stream, or a non-triangle index count.  Copy through so the
     * caller always gets a valid (if unreduced) buffer. */
    if (!indices || !positions || vertex_count == 0 ||
        index_count < 3 || (index_count % 3) != 0 ||
        position_stride_bytes < 3 * sizeof(float)) {
        return lod_passthrough(indices, index_count, out_indices);
    }

    /* A ratio that keeps everything (or is nonsensical) is a passthrough. */
    if (!(target_ratio > 0.0f) || target_ratio >= 1.0f) {
        return lod_passthrough(indices, index_count, out_indices);
    }

    /* Target triangle count -> target index count (multiple of 3). */
    size_t base_tris = index_count / 3;
    size_t target_tris = (size_t)((double)base_tris * (double)target_ratio);
    if (target_tris < 1) target_tris = 1;
    size_t target_index_count = target_tris * 3;
    if (target_index_count >= index_count) {
        /* Rounding pushed us to (or above) the original — nothing to do. */
        return lod_passthrough(indices, index_count, out_indices);
    }

    /* ---- topology-preserving simplification ----------------------------- */
    float result_error = 0.0f;
    size_t simplified =
        meshopt_simplify(out_indices, indices, index_count,
                         positions, vertex_count, position_stride_bytes,
                         target_index_count, target_error,
                         /*options*/ 0u, &result_error);

    /* Defensive: meshopt never returns more than the input, but clamp so a
     * future library change can never overrun the caller's buffer. */
    if (simplified > index_count) simplified = index_count;

    /* ---- aggressive fallback (sloppy) ----------------------------------- *
     * meshopt_simplify preserves topology and can stop well short of the
     * target to avoid damaging the mesh.  When the requested LOD is
     * aggressive (<= 50% kept) AND the topology-preserving result is still
     * far above the target (more than 1.5x the requested triangle count),
     * fall back to meshopt_simplifySloppy, which ignores topology to hit
     * the budget.  The sloppy result is accepted only if it is valid and
     * actually reduces further than the topology-preserving pass. */
    if (target_ratio <= 0.5f && simplified > (target_index_count * 3u) / 2u) {
        /* Sloppy writes at most index_count indices; reuse a scratch buffer
         * so we never clobber a good result with a worse one. */
        unsigned int *scratch =
            (unsigned int *)JCE_MALLOC(index_count * sizeof(unsigned int));
        if (scratch) {
            float sloppy_error = 0.0f;
            size_t sloppy =
                meshopt_simplifySloppy(scratch, indices, index_count,
                                       positions, vertex_count,
                                       position_stride_bytes,
                                       /*vertex_lock*/ NULL,
                                       target_index_count, target_error,
                                       &sloppy_error);
            if (sloppy >= 3 && (sloppy % 3) == 0 &&
                sloppy < simplified && sloppy <= index_count) {
                memcpy(out_indices, scratch, sloppy * sizeof(unsigned int));
                simplified = sloppy;
            }
            JCE_FREE(scratch);
        }
    }

    /* If the simplifier produced nothing usable, fall back to the base mesh
     * so a level is never empty/degenerate. */
    if (simplified < 3 || (simplified % 3) != 0) {
        return lod_passthrough(indices, index_count, out_indices);
    }

    return simplified;
}

size_t jce_mesh_generate_lod_chain(const float        *positions,
                                   size_t              vertex_count,
                                   size_t              position_stride_bytes,
                                   const unsigned int *base_indices,
                                   size_t              base_index_count,
                                   const float        *target_ratios,
                                   size_t              level_count,
                                   unsigned int      **out_level_indices,
                                   size_t             *out_level_index_counts)
{
    if (!out_level_indices || !out_level_index_counts) return 0;

    /* Clamp the requested level count and pre-clear the output slots so the
     * contract (out[i] == NULL for i >= return) always holds. */
    if (level_count > JCE_MESH_LOD_MAX_LEVELS)
        level_count = JCE_MESH_LOD_MAX_LEVELS;
    for (size_t i = 0; i < level_count; ++i) {
        out_level_indices[i]      = NULL;
        out_level_index_counts[i] = 0;
    }

    if (level_count == 0 || !target_ratios ||
        !base_indices || base_index_count < 3) {
        return 0;
    }

    size_t produced = 0;
    for (size_t lvl = 0; lvl < level_count; ++lvl) {
        /* Each level is simplified from the BASE mesh (not chained), so the
         * scratch buffer is sized to the base index count. */
        unsigned int *buf =
            (unsigned int *)JCE_MALLOC(base_index_count * sizeof(unsigned int));
        if (!buf) {
            /* Allocation failure: free everything produced so far and hand
             * back nothing (no partial ownership). */
            for (size_t j = 0; j < produced; ++j) {
                JCE_FREE(out_level_indices[j]);
                out_level_indices[j]      = NULL;
                out_level_index_counts[j] = 0;
            }
            return 0;
        }

        size_t n = jce_mesh_simplify(positions, vertex_count,
                                     position_stride_bytes,
                                     base_indices, base_index_count,
                                     target_ratios[lvl], 0.01f, buf);

        out_level_indices[produced]      = buf;
        out_level_index_counts[produced] = n;
        ++produced;
    }

    return produced;
}

void jce_mesh_lod_chain_free(unsigned int **level_indices, size_t level_count)
{
    if (!level_indices) return;
    for (size_t i = 0; i < level_count; ++i) {
        if (level_indices[i]) {
            JCE_FREE(level_indices[i]);
            level_indices[i] = NULL;
        }
    }
}
