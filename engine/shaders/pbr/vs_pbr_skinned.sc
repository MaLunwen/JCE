$input a_position, a_normal, a_tangent, a_texcoord0, a_indices, a_weight
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent

#include <bgfx_shader.sh>

void main()
{
    // Bone blending via model palette
    mat4 skinMtx = a_weight.x * u_model[int(a_indices.x)]
                 + a_weight.y * u_model[int(a_indices.y)]
                 + a_weight.z * u_model[int(a_indices.z)]
                 + a_weight.w * u_model[int(a_indices.w)];

    vec3 wpos = mul(skinMtx, vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));

    v_normal    = normalize(mul(skinMtx, vec4(a_normal, 0.0)).xyz);
    v_tangent   = normalize(mul(skinMtx, vec4(a_tangent.xyz, 0.0)).xyz);
    v_bitangent = cross(v_normal, v_tangent) * a_tangent.w;
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
}
