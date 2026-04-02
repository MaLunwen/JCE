$input a_position, a_indices, a_weight
$output v_texcoord0

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
    v_texcoord0 = vec2(0.0, 0.0);
}
