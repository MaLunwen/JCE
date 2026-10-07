/*
 * cs_hiz_build.sc -- Hi-Z (hierarchical-Z) max-depth pyramid build pass
 * (large-world #5).  One dispatch per destination mip.
 *
 * Each thread owns one destination texel and MAX-reduces its source footprint
 * into it (depth: 0=near, 1=far; max = farthest occluder over the footprint).
 * Pass 0 reads the scene depth-prepass texture (s_src bound to the depth buffer,
 * LOD 0). Later levels use cs_hiz_reduce with distinct image subresources.
 * Normalized footprints overlap at odd-sized boundaries: every source texel
 * touched by a destination interval contributes to its conservative maximum.
 *
 * The cull shaders consume the resulting pyramid via their INLINED Hi-Z test
 * (cs_cull_frustum / cs_cull_compact / cs_foliage_cull — a shared helper taking
 * s_hiz as an argument crashed the GL driver, revert d7d785c4).  The scalar
 * math mirrors tests/renderer/test_jce_hiz_occlusion.c; keep them in sync.
 *
 * u_hiz_build = { dst_width, dst_height, src_width, src_height }
 */

#include <bgfx_compute.sh>

SAMPLER2D(s_src, 0);            /* depth buffer, LOD 0 */
IMAGE2D_WO(i_dst, r32f, 1);     /* destination Hi-Z mip */

uniform vec4 u_hiz_build;

NUM_THREADS(8, 8, 1)
void main()
{
    ivec2 d = ivec2(gl_GlobalInvocationID.xy);
    if (float(d.x) >= u_hiz_build.x || float(d.y) >= u_hiz_build.y) {
        return;
    }

    ivec2 dstDim = ivec2(u_hiz_build.xy);
    ivec2 srcDim = ivec2(u_hiz_build.zw);
    ivec2 lo = d * srcDim / dstDim;
    ivec2 hi = ((d + ivec2(1, 1)) * srcDim + dstDim - ivec2(1, 1)) / dstDim;
    float mx = 0.0;
    /* floor(n/2) destination dimensions need at most three source texels per
     * axis. Integer bounds also handle a one-texel axis without out-of-bounds. */
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            ivec2 p = lo + ivec2(x, y);
            if (p.x < hi.x && p.y < hi.y) {
                vec2 uv = (vec2(p) + vec2(0.5, 0.5)) / vec2(srcDim);
                mx = max(mx, texture2DLod(s_src, uv, 0.0).x);
            }
        }
    }
    imageStore(i_dst, d, vec4(mx, 0.0, 0.0, 0.0));
}
