/*
 * hiz_occlusion.sh -- Hi-Z (hierarchical-Z) occlusion test for the GPU cull
 * (large-world #5).  Shared include for cs_cull_frustum / cs_cull_compact.
 *
 * THE SCALAR MATH MIRRORS tests/renderer/test_jce_hiz_occlusion.c (verified 6/6).
 * Keep the two in sync.
 *
 * s_hiz is a MAX-depth pyramid of the scene depth prepass (depth: 0=near,1=far;
 * the renderer pre-remaps GL [-1,1] -> [0,1] when building it so this test is
 * backend-uniform).  An instance's world AABB is OCCLUDED iff the nearest point
 * of its screen footprint is farther than the farthest occluder over that
 * footprint:  aabb_min_depth > max(HiZ over footprint).
 *
 * Conservative (returns false = VISIBLE, never wrongly culled) when the AABB:
 *   - straddles the near plane (any corner w<=0),
 *   - is fully off-screen (frustum cull owns that case in-pipeline anyway),
 *   - has a corner in front of the near plane (ndc.z<0), or
 *   - is larger than the coarsest pyramid level.
 *
 * hiz_params = { level0_width, level0_height, num_mips, 0 }.
 */
#ifndef HIZ_OCCLUSION_SH
#define HIZ_OCCLUSION_SH

bool hiz_aabb_occluded(BgfxSampler2D s_hiz, vec4 hiz_params, mat4 viewProj,
                       vec3 center, vec3 extent)
{
    vec2  mn_uv = vec2(1e9, 1e9);
    vec2  mx_uv = vec2(-1e9, -1e9);
    float mn_z  = 1e9;

    for (int c = 0; c < 8; ++c) {
        vec3 corner = center + vec3(
            (c & 1) != 0 ? extent.x : -extent.x,
            (c & 2) != 0 ? extent.y : -extent.y,
            (c & 4) != 0 ? extent.z : -extent.z);
        vec4 clip = mul(viewProj, vec4(corner, 1.0));
        if (clip.w <= 1e-6) return false;            /* near-plane straddle */
        vec3 ndc = clip.xyz / clip.w;
        vec2 uv  = ndc.xy * 0.5 + 0.5;
        mn_uv = min(mn_uv, uv);
        mx_uv = max(mx_uv, uv);
        mn_z  = min(mn_z, ndc.z);
    }
    if (mx_uv.x < 0.0 || mn_uv.x > 1.0 || mx_uv.y < 0.0 || mn_uv.y > 1.0)
        return false;                                /* off-screen */
    if (mn_z < 0.0) return false;                    /* in front of near */

    /* pick the mip whose texels are ~the footprint size (<= 2 texels span). */
    float span = max((mx_uv.x - mn_uv.x) * hiz_params.x,
                     (mx_uv.y - mn_uv.y) * hiz_params.y);
    float mip  = ceil(log2(max(span, 1.0)));
    if (mip >= hiz_params.z) return false;           /* bigger than pyramid */

    /* max occluder depth over the footprint corners at that mip. */
    float occ = texture2DLod(s_hiz, vec2(mn_uv.x, mn_uv.y), mip).r;
    occ = max(occ, texture2DLod(s_hiz, vec2(mx_uv.x, mn_uv.y), mip).r);
    occ = max(occ, texture2DLod(s_hiz, vec2(mn_uv.x, mx_uv.y), mip).r);
    occ = max(occ, texture2DLod(s_hiz, vec2(mx_uv.x, mx_uv.y), mip).r);

    return mn_z > occ;                               /* nearest pt behind occluder */
}

#endif /* HIZ_OCCLUSION_SH */
