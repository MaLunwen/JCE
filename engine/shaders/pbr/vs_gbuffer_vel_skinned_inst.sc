$input a_position, a_normal, a_indices, a_weight, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_normal, v_curClip, v_prevClip

#include <bgfx_shader.sh>

/*
 * vs_gbuffer_vel_skinned_inst.sc — GPU-crowd sibling of vs_gbuffer_vel_skinned.
 *
 * Skins the vertex TWICE from the shared per-frame bone TEXTURES — s_bones
 * (stage 4, this frame's palettes) and s_prevBones (stage 5, last frame's
 * palettes packed at the SAME per-instance base) — so one instanced submit
 * writes true per-limb motion vectors + world normals for an entire animated
 * crowd.  Per-instance stream mirrors the crowd color/shadow draws: world
 * matrix (i_data0..3) + palette base in bones (i_data4.x).  Palettes are
 * skeleton-LOCAL; the (static this frame — the batcher's `moved` check routes
 * world-moved chars per-char) world matrix is applied after each skin, and the
 * object's motion therefore comes from the bone delta + camera via
 * u_prevViewProj.  Fragment side reuses fs_gbuffer_vel unchanged.  Portable
 * texture2DLod reads (no texelFetch), LBS as transform-then-blend (lint E002).
 */

SAMPLER2D(s_bones,     4);
/* Stage 6, not 5: stage 5 is the engine-wide s_shadowMap slot.  Nothing
 * binds shadows for the velocity pass today, so 5 worked -- which is
 * precisely the shape of the water bug this rule now prevents: the slot
 * conflict was created later, by adding the feature that was missing. */
SAMPLER2D(s_prevBones, 6);
uniform vec4 u_boneTexParams;   /* x = texWidth, y = texHeight (both textures) */
uniform mat4 u_curViewProj;
uniform mat4 u_prevViewProj;

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
    float max_idx = float(BGFX_CONFIG_MAX_BONES - 1);
    if (idx > max_idx) {
        idx = max_idx;
    }
    return idx;
}

vec2 bone_uv(float elem)
{
    float W = u_boneTexParams.x;
    float col = mod(elem, W);
    float row = floor(elem / W);
    return vec2((col + 0.5) / W, (row + 0.5) / u_boneTexParams.y);
}

mat4 bone_matrix_cur(float boneIdx, float paletteBase)
{
    float e = (paletteBase + boneIdx) * 4.0;
    return mtxFromCols(texture2DLod(s_bones, bone_uv(e),       0.0),
                       texture2DLod(s_bones, bone_uv(e + 1.0), 0.0),
                       texture2DLod(s_bones, bone_uv(e + 2.0), 0.0),
                       texture2DLod(s_bones, bone_uv(e + 3.0), 0.0));
}

mat4 bone_matrix_prev(float boneIdx, float paletteBase)
{
    float e = (paletteBase + boneIdx) * 4.0;
    return mtxFromCols(texture2DLod(s_prevBones, bone_uv(e),       0.0),
                       texture2DLod(s_prevBones, bone_uv(e + 1.0), 0.0),
                       texture2DLod(s_prevBones, bone_uv(e + 2.0), 0.0),
                       texture2DLod(s_prevBones, bone_uv(e + 3.0), 0.0));
}

void main()
{
    mat4 world  = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    float pbase = i_data4.x;

    float i0 = decode_bone_index_clamped(a_indices.x);
    float i1 = decode_bone_index_clamped(a_indices.y);
    float i2 = decode_bone_index_clamped(a_indices.z);
    float i3 = decode_bone_index_clamped(a_indices.w);

    vec4 lp = vec4(a_position, 1.0);
    vec4 ln = vec4(a_normal, 0.0);

    mat4 c0 = bone_matrix_cur(i0, pbase);
    mat4 c1 = bone_matrix_cur(i1, pbase);
    mat4 c2 = bone_matrix_cur(i2, pbase);
    mat4 c3 = bone_matrix_cur(i3, pbase);
    vec3 curSkinP = a_weight.x * mul(c0, lp).xyz + a_weight.y * mul(c1, lp).xyz
                  + a_weight.z * mul(c2, lp).xyz + a_weight.w * mul(c3, lp).xyz;
    vec3 curSkinN = a_weight.x * mul(c0, ln).xyz + a_weight.y * mul(c1, ln).xyz
                  + a_weight.z * mul(c2, ln).xyz + a_weight.w * mul(c3, ln).xyz;

    mat4 p0 = bone_matrix_prev(i0, pbase);
    mat4 p1 = bone_matrix_prev(i1, pbase);
    mat4 p2 = bone_matrix_prev(i2, pbase);
    mat4 p3 = bone_matrix_prev(i3, pbase);
    vec3 prevSkinP = a_weight.x * mul(p0, lp).xyz + a_weight.y * mul(p1, lp).xyz
                   + a_weight.z * mul(p2, lp).xyz + a_weight.w * mul(p3, lp).xyz;

    vec3 curWpos  = mul(world, vec4(curSkinP,  1.0)).xyz;
    vec3 prevWpos = mul(world, vec4(prevSkinP, 1.0)).xyz;

    vec4 curClip  = mul(u_curViewProj,  vec4(curWpos,  1.0));
    vec4 prevClip = mul(u_prevViewProj, vec4(prevWpos, 1.0));

    gl_Position = curClip;
    v_curClip   = curClip;
    v_prevClip  = prevClip;
    v_normal    = normalize(mul(world, vec4(curSkinN, 0.0)).xyz);
}
