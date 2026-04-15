$input a_position, a_normal, a_tangent, a_texcoord0, a_indices, a_weight
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent

#include <bgfx_shader.sh>

float decode_bone_index(float raw_index)
{
    float idx = raw_index;
    if (idx <= 1.0) {
        idx *= 255.0;
    }
    return floor(idx + 0.5);
}

void main()
{
    int i0 = clamp(int(decode_bone_index(a_indices.x)), 0, 63);
    int i1 = clamp(int(decode_bone_index(a_indices.y)), 0, 63);
    int i2 = clamp(int(decode_bone_index(a_indices.z)), 0, 63);
    int i3 = clamp(int(decode_bone_index(a_indices.w)), 0, 63);

    // Bone blending via model palette
    mat4 skinMtx = a_weight.x * u_model[i0]
                 + a_weight.y * u_model[i1]
                 + a_weight.z * u_model[i2]
                 + a_weight.w * u_model[i3];

    vec3 wpos = mul(skinMtx, vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));

    v_normal    = normalize(mul(skinMtx, vec4(a_normal, 0.0)).xyz);
    v_tangent   = normalize(mul(skinMtx, vec4(a_tangent.xyz, 0.0)).xyz);
    v_bitangent = cross(v_normal, v_tangent) * a_tangent.w;
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
}
