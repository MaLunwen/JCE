/*
 * jce_lightmap_uv.h  Lightmap chart packer.
 *
 * Given a mesh's first-channel UVs + triangle adjacency, partition
 * triangles into "charts" (connected components of triangles with
 * similar normals), pack each chart into a rectangular lightmap
 * atlas, and emit a UV2 channel where each chart is a non-
 * overlapping rect.
 *
 * Algorithm: a simple greedy "shelf" packer — sort charts by height
 * descending, place left-to-right on shelves; open a new shelf when
 * the current row overflows.  No external dependency (xatlas-style,
 * but much smaller).
 *
 * Output is uv2[vertex_count * 2].  Caller owns the buffer.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_LIGHTMAP_UV_H
#define JCE_LIGHTMAP_UV_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    const float    *positions;     /* vertex_count * 3 */
    const float    *normals;       /* vertex_count * 3 */
    const float    *uvs;           /* vertex_count * 2 (existing UV0) */
    uint32_t        vertex_count;
    const uint32_t *indices;       /* triangle_count * 3 */
    uint32_t        triangle_count;
} JceLightmapUvInput;

typedef struct {
    /* Padding around each chart, in atlas pixels. */
    uint32_t padding_px;
    /* Atlas dimensions (must be > 0).  Charts will be normalised
     * into [0,1] UV2 after packing. */
    uint32_t atlas_width;
    uint32_t atlas_height;
    /* Cosine of the angle between adjacent triangle normals that
     * keeps them in the same chart (default cos(45deg) ≈ 0.707). */
    float    chart_angle_cos;
} JceLightmapUvOptions;

typedef struct {
    /* Pre-allocated buffer of size vertex_count * 2.  Filled with
     * normalised [0,1] UV2 coordinates. */
    float   *out_uv2;
    /* Filled with the number of distinct charts produced. */
    uint32_t chart_count;
    /* Atlas utilisation ratio (covered area / total area).  Useful
     * to surface in the bake panel. */
    float    utilisation;
} JceLightmapUvResult;

/* Generate the UV2 channel.  Returns false on bad input.  The caller
 * is responsible for ensuring `result->out_uv2` is allocated with
 * `vertex_count * 2 * sizeof(float)`. */
JCE_API bool jce_lightmap_uv_generate(const JceLightmapUvInput   *input,
                                       const JceLightmapUvOptions *opts,
                                       JceLightmapUvResult        *result);

JCE_EXTERN_C_END

#endif /* JCE_LIGHTMAP_UV_H */
