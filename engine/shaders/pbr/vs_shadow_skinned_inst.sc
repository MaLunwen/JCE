$input a_position, a_indices, a_weight, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0

#include <bgfx_shader.sh>
#include "shadow_pancake.sh"

/* Instanced SKINNED shadow vertex shader — GPU crowd instancing, depth-only.
 *
 * The depth sibling of vs_pbr_skinned_inst.sc: each instance carries its world
 * matrix (i_data0..3) and its bone-palette BASE offset (i_data4.x, in bones)
 * into the shared per-frame RGBA32F bone texture, so one instanced submit casts
 * an entire same-mesh animated crowd into a cascade.  The palette is
 * skeleton-LOCAL (as packed from SrAnimInstance.skin_palette); the world matrix
 * places the deformed silhouette exactly where the color crowd pass draws it.
 *
 * s_bones rides sampler stage 4 (aliasing s_emissive — see
 * vs_pbr_skinned_inst.sc for why: stages 9-12 belong to the CSM cascades and
 * are re-bound by the color pre-submit hook; the shadow pass has no material
 * samplers at all, so stage 4 is simply free here, and sharing the stage keeps
 * ONE uniform/binding convention for both programs).  Portable data-texture
 * read: texture2DLod at texel centres, no texelFetch (GLSL 120 / WebGL2). */

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
    float max_idx = float(BGFX_CONFIG_MAX_BONES - 1);
    if (idx > max_idx) {
        idx = max_idx;
    }
    return idx;
}

vec4 bone_texel(float elem)
{
    float W = u_boneTexParams.x;
    float col = mod(elem, W);
    float row = floor(elem / W);
    vec2 uv = vec2((col + 0.5) / W, (row + 0.5) / u_boneTexParams.y);
    return texture2DLod(s_bones, uv, 0.0);
}

mat4 bone_matrix(float boneIdx, float paletteBase)
{
    float e = (paletteBase + boneIdx) * 4.0;
    return mtxFromCols(bone_texel(e),        bone_texel(e + 1.0),
                       bone_texel(e + 2.0),  bone_texel(e + 3.0));
}

void main()
{
    mat4 world  = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    float pbase = i_data4.x;

    float i0 = decode_bone_index_clamped(a_indices.x);
    float i1 = decode_bone_index_clamped(a_indices.y);
    float i2 = decode_bone_index_clamped(a_indices.z);
    float i3 = decode_bone_index_clamped(a_indices.w);

    mat4 b0 = bone_matrix(i0, pbase);
    mat4 b1 = bone_matrix(i1, pbase);
    mat4 b2 = bone_matrix(i2, pbase);
    mat4 b3 = bone_matrix(i3, pbase);

    /* LBS as transform-then-blend (scalar*vec, not scalar*mat — lint E002;
     * numerically identical: (Sum w_i M_i) v == Sum w_i (M_i v)). */
    vec4 lp = vec4(a_position, 1.0);
    vec3 sp = a_weight.x * mul(b0, lp).xyz + a_weight.y * mul(b1, lp).xyz
            + a_weight.z * mul(b2, lp).xyz + a_weight.w * mul(b3, lp).xyz;

    vec3 wpos   = mul(world, vec4(sp, 1.0)).xyz;
    /* Clamped to the near plane, not rejected by it -- see
     * shadow_pancake.sh.  A caster further up-sun than the cascade
     * box reaches would otherwise be clipped away entirely and the
     * shadow it owes would be missing. */
    gl_Position = jce_shadow_pancake(mul(u_viewProj, vec4(wpos, 1.0)));
    v_texcoord0 = vec2(0.0, 0.0);
}
