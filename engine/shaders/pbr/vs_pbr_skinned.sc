$input a_position, a_normal, a_tangent, a_texcoord0, a_indices, a_weight
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

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
    vec4 viewPos = mul(u_view, vec4(wpos, 1.0));
    gl_Position = mul(u_proj, viewPos);

    v_normal    = normalize(mul(skinMtx, vec4(a_normal, 0.0)).xyz);
    v_tangent   = normalize(mul(skinMtx, vec4(a_tangent.xyz, 0.0)).xyz);
    v_bitangent = cross(v_normal, v_tangent) * a_tangent.w;
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
    v_localpos  = a_position;
    v_viewdepth = -viewPos.z;
}
