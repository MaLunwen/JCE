$input a_position, a_indices, a_weight
$output v_texcoord0

#include <bgfx_shader.sh>

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
    if (idx > 63.0) {
        idx = 63.0;
    }
    return int(idx);
}

void main()
{
    int i0 = decode_bone_index_clamped(a_indices.x);
    int i1 = decode_bone_index_clamped(a_indices.y);
    int i2 = decode_bone_index_clamped(a_indices.z);
    int i3 = decode_bone_index_clamped(a_indices.w);

    // Bone blending via model palette
    mat4 skinMtx = a_weight.x * u_model[i0]
                 + a_weight.y * u_model[i1]
                 + a_weight.z * u_model[i2]
                 + a_weight.w * u_model[i3];

    vec3 wpos = mul(skinMtx, vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));
    v_texcoord0 = vec2(0.0, 0.0);
}
