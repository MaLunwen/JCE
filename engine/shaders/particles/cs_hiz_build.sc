/*
 * cs_hiz_build.sc -- Hi-Z (hierarchical-Z) max-depth pyramid build pass
 * (large-world #5).  One dispatch per destination mip.
 *
 * Each thread owns one destination texel and 2x2-MAX-reduces the source level
 * into it (depth: 0=near, 1=far; max = farthest occluder over the footprint).
 * Pass 0 reads the scene depth-prepass texture (s_src bound to the depth buffer,
 * src_lod = 0); passes 1..N read the previous Hi-Z mip (src_lod = mip-1).  The
 * source is read through a SAMPLER (works for both the depth texture and the
 * Hi-Z r32f mips); the destination mip is bound as a write image.
 *
 * The cull shader (hiz_occlusion.sh) consumes the resulting pyramid.
 *
 * u_hiz_build = { dst_width, dst_height, src_lod, 0 }
 */

#include <bgfx_compute.sh>

SAMPLER2D(s_src, 0);            /* depth buffer (pass 0) or Hi-Z mip-1 (pass>0) */
IMAGE2D_WO(i_dst, r32f, 1);     /* destination Hi-Z mip */

uniform vec4 u_hiz_build;

NUM_THREADS(8, 8, 1)
void main()
{
    ivec2 d = ivec2(gl_GlobalInvocationID.xy);
    if (float(d.x) >= u_hiz_build.x || float(d.y) >= u_hiz_build.y) {
        return;
    }

    /* The source level is exactly twice the destination's size; sample the 2x2
     * source-texel block covering this destination texel at the source LOD. */
    vec2  srcDim = vec2(u_hiz_build.x, u_hiz_build.y) * 2.0;
    float lod    = u_hiz_build.z;
    vec2  b      = vec2(d) * 2.0;

    float s0 = texture2DLod(s_src, (b + vec2(0.5, 0.5)) / srcDim, lod).x;
    float s1 = texture2DLod(s_src, (b + vec2(1.5, 0.5)) / srcDim, lod).x;
    float s2 = texture2DLod(s_src, (b + vec2(0.5, 1.5)) / srcDim, lod).x;
    float s3 = texture2DLod(s_src, (b + vec2(1.5, 1.5)) / srcDim, lod).x;

    float mx = max(max(s0, s1), max(s2, s3));
    imageStore(i_dst, d, vec4(mx, 0.0, 0.0, 0.0));
}
