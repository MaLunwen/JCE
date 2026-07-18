/*
 * cs_hiz_reduce.sc -- Hi-Z MAX-depth pyramid mip reduce (large-world #5, full
 * pyramid slice).  Builds Hi-Z mip i (i>=1) as the 2x2-MAX of mip i-1.
 *
 * Unlike cs_hiz_build (which reads the depth prepass through a SAMPLER for mip 0),
 * this reads the SOURCE mip as a read-only IMAGE and writes the DEST mip as a
 * write image — BOTH are distinct mips (subresources) of the SAME Hi-Z texture,
 * which D3D11/D3D12 permit (no SRV/UAV aliasing: a sampler view would cover ALL
 * mips including the one being written and get unbound).  depth: 0=near, 1=far;
 * MAX keeps the FARTHEST occluder over the footprint so the cull is conservative.
 *
 * Odd-sized source levels: imageLoad past the edge returns 0 (=near), which only
 * pulls the MAX toward near → UNDER-estimates the occluder → keeps the instance
 * VISIBLE → conservative (never a false occlusion at a boundary).
 *
 * u_hiz_build = { dst_width, dst_height, 0, 0 }
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

    ivec2 b = d * 2;
    float s0 = imageLoad(i_src, b + ivec2(0, 0)).x;
    float s1 = imageLoad(i_src, b + ivec2(1, 0)).x;
    float s2 = imageLoad(i_src, b + ivec2(0, 1)).x;
    float s3 = imageLoad(i_src, b + ivec2(1, 1)).x;

    float mx = max(max(s0, s1), max(s2, s3));
    imageStore(i_dst, d, vec4(mx, 0.0, 0.0, 0.0));
}
