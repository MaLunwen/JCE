$input a_position, a_normal, a_indices, a_weight
$output v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_outlineParams;  // x = outline width (world units)

float decode_bone_index(float raw_index)
{
    float idx = raw_index;
    if (idx <= 1.0) {
        idx *= 255.0;
    }
    return floor(idx + 0.5);
}

int decode_bone_index_clamped(float raw_index)
{
    float idx = decode_bone_index(raw_index);
    if (idx < 0.0) {
        idx = 0.0;
    }
    float max_idx = float(BGFX_CONFIG_MAX_BONES - 1);
    if (idx > max_idx) {
        idx = max_idx;
    }
    return int(idx);
}

void main()
{
    int i0 = decode_bone_index_clamped(a_indices.x);
    int i1 = decode_bone_index_clamped(a_indices.y);
    int i2 = decode_bone_index_clamped(a_indices.z);
    int i3 = decode_bone_index_clamped(a_indices.w);

    mat4 skinMtx = a_weight.x * u_model[i0]
                 + a_weight.y * u_model[i1]
                 + a_weight.z * u_model[i2]
                 + a_weight.w * u_model[i3];

    vec3 wpos = mul(skinMtx, vec4(a_position, 1.0)).xyz;
    // Skinned world-space normal (mul(mtx,vec) — no raw mat3 ctor / GL TBN bug).
    vec3 wnrm = normalize(mul(skinMtx, vec4(a_normal, 0.0)).xyz);
    wpos += wnrm * u_outlineParams.x;

    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));
    v_texcoord0 = vec2(0.0, 0.0);
}
