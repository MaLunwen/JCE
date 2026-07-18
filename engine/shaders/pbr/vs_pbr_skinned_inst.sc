$input a_position, a_normal, a_tangent, a_texcoord0, a_indices, a_weight, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

#include <bgfx_shader.sh>

/* Instanced skinned PBR vertex shader — GPU crowd instancing.
 *
 * The bone palette for EVERY visible skinned character sharing this mesh is
 * packed into ONE per-frame RGBA32F texture (s_bones): each bone matrix = 4
 * consecutive texels (its 4 columns, in the same order bgfx_set_transform
 * uploads into u_model[]).  The per-instance palette BASE offset (in bones)
 * arrives in i_data0.x, so a single instanced draw renders an entire crowd of
 * the same mesh — each instance reading its own pose from the texture.  This
 * collapses the per-character skinned draw wall (measured 4000 Fox = 16454
 * draws) to one draw per (mesh, view).
 *
 * Portable data-texture read (mirrors the Forward+ cluster fetch in
 * fs_pbr_body.sh): texture2DLod at the texel CENTRE with an explicit LOD 0 —
 * no texelFetch, so the same path works on GLSL 120 / WebGL2 (charter: the C
 * side falls back to the per-character skinned program on tiers without
 * vertex-shader texture sampling).  The palette is already WORLD-space (the
 * world transform is folded in at palette-build time), so there is no separate
 * per-instance model matrix — skinMtx maps local -> world directly, exactly as
 * vs_pbr_skinned.sc does with u_model[].
 */

/* Stage 4 — REUSED from s_emissive (SSAO's s_aoMap-reuse precedent): all 16
 * sampler stages of the fs_pbr program family are occupied (0-8 material+IBL,
 * 9-12 CSM cascades, 13 cookie, 14 cluster/IES, 15 local shadow) and the CSM
 * pre-submit re-bind stomps stages 9-12 after any earlier bind — the original
 * slot-9 s_bones read exactly that cascade texture (degenerate matrices,
 * invisible crowd).  Stage 4's fragment sample is multiplied by
 * u_emissiveFactor.xyz, so for NON-emissive materials (factor 0 — the crowd
 * eligibility gate) the aliased bone texture contributes nothing to shading. */
SAMPLER2D(s_bones, 4);
uniform vec4 u_boneTexParams;   /* x = texWidth, y = texHeight (in texels) */

float decode_bone_index(float raw_index)
{
    float idx = raw_index;
    if (idx <= 1.0) {
        idx *= 255.0;
    }
    return floor(idx + 0.5);
}

float decode_bone_index_clamped(float raw_index)
{
    float idx = decode_bone_index(raw_index);
    if (idx < 0.0) {
        idx = 0.0;
    }
    /* Clamp to the last valid palette slot (see vs_pbr_skinned.sc). */
    float max_idx = float(BGFX_CONFIG_MAX_BONES - 1);
    if (idx > max_idx) {
        idx = max_idx;
    }
    return idx;
}

/* Fetch RGBA32F texel `elem` from the bone texture (row-major, W texels wide).
 * NEAREST point-clamp sampling at the texel centre is exact for integer elem. */
vec4 bone_texel(float elem)
{
    float W = u_boneTexParams.x;
    float col = mod(elem, W);
    float row = floor(elem / W);
    vec2 uv = vec2((col + 0.5) / W, (row + 0.5) / u_boneTexParams.y);
    return texture2DLod(s_bones, uv, 0.0);
}

/* Reconstruct a bone's world matrix: 4 consecutive texels = its 4 columns
 * (mtxFromCols handles HLSL/GLSL row/col conventions, matching vs_pbr_inst.sc). */
mat4 bone_matrix(float boneIdx, float paletteBase)
{
    float e = (paletteBase + boneIdx) * 4.0;
    return mtxFromCols(bone_texel(e),        bone_texel(e + 1.0),
                       bone_texel(e + 2.0),  bone_texel(e + 3.0));
}

void main()
{
    /* Per-instance world matrix (i_data0..3, same packing as vs_pbr_inst) and
     * bone palette base offset (i_data4.x).  The bone palette in the texture is
     * model-LOCAL (skeleton space, as stored in SrAnimInstance); the world
     * transform — position/scale of this crowd member — is applied here, exactly
     * as jce_model_draw folds `model` into the per-character upload. */
    mat4 world = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    float pbase = i_data4.x;

    float i0 = decode_bone_index_clamped(a_indices.x);
    float i1 = decode_bone_index_clamped(a_indices.y);
    float i2 = decode_bone_index_clamped(a_indices.z);
    float i3 = decode_bone_index_clamped(a_indices.w);

    mat4 b0 = bone_matrix(i0, pbase);
    mat4 b1 = bone_matrix(i1, pbase);
    mat4 b2 = bone_matrix(i2, pbase);
    mat4 b3 = bone_matrix(i3, pbase);

    /* Linear-blend skinning as TRANSFORM-then-blend: blend the transformed
     * vectors (scalar * vec) rather than the matrices (scalar * mat), which is
     * numerically identical for LBS — (Sum w_i M_i) v == Sum w_i (M_i v) — but
     * keeps every matrix op a mul() (the shader lint rejects scalar*matrix
     * because HLSL treats '*' component-wise). */
    vec4 lp = vec4(a_position, 1.0);
    vec4 ln = vec4(a_normal, 0.0);
    vec4 lt = vec4(a_tangent.xyz, 0.0);

    /* Skinned in MODEL space, then transformed to world by the instance matrix. */
    vec3 sp = a_weight.x * mul(b0, lp).xyz + a_weight.y * mul(b1, lp).xyz
            + a_weight.z * mul(b2, lp).xyz + a_weight.w * mul(b3, lp).xyz;
    vec3 sn = a_weight.x * mul(b0, ln).xyz + a_weight.y * mul(b1, ln).xyz
            + a_weight.z * mul(b2, ln).xyz + a_weight.w * mul(b3, ln).xyz;
    vec3 st = a_weight.x * mul(b0, lt).xyz + a_weight.y * mul(b1, lt).xyz
            + a_weight.z * mul(b2, lt).xyz + a_weight.w * mul(b3, lt).xyz;

    vec3 wpos = mul(world, vec4(sp, 1.0)).xyz;
    vec3 wnrm = mul(world, vec4(sn, 0.0)).xyz;
    vec3 wtan = mul(world, vec4(st, 0.0)).xyz;

    vec4 viewPos = mul(u_view, vec4(wpos, 1.0));
    gl_Position = mul(u_proj, viewPos);

    v_normal    = normalize(wnrm);
    v_tangent   = normalize(wtan);
    v_bitangent = cross(v_normal, v_tangent) * a_tangent.w;
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
    v_localpos  = a_position;
    v_viewdepth = -viewPos.z;
}
