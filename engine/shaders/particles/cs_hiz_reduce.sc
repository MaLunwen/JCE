/*
 * cs_hiz_reduce.sc -- Hi-Z MAX-depth pyramid mip reduce (large-world #5, full
 * pyramid slice). Builds Hi-Z mip i (i>=1) from its source footprint in mip i-1.
 *
 * Unlike cs_hiz_build (which reads the depth prepass through a SAMPLER for mip 0),
 * this reads the SOURCE mip as a read-only IMAGE and writes the DEST mip as a
 * write image — BOTH are distinct mips (subresources) of the SAME Hi-Z texture,
 * which D3D11/D3D12 permit (no SRV/UAV aliasing: a sampler view would cover ALL
 * mips including the one being written and get unbound).  depth: 0=near, 1=far;
 * MAX keeps the FARTHEST occluder over the footprint so the cull is conservative.
 *
 * Odd-sized normalized footprints overlap. Dropping a source texel can lower
 * the maximum and falsely occlude visible geometry; no edge texel may be lost.
 *
 * u_hiz_build = { dst_width, dst_height, src_width, src_height }
 */

#include <bgfx_compute.sh>

IMAGE2D_RO(i_src, r32f, 0);    /* Hi-Z mip i-1 (read) */
IMAGE2D_WO(i_dst, r32f, 1);    /* Hi-Z mip i   (write) */

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
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) {
            ivec2 p = lo + ivec2(x, y);
            if (p.x < hi.x && p.y < hi.y) {
                mx = max(mx, imageLoad(i_src, p).x);
            }
        }
    }
    imageStore(i_dst, d, vec4(mx, 0.0, 0.0, 0.0));
}
