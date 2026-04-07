$input a_position, a_normal, a_tangent, a_texcoord0
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent

#include <bgfx_shader.sh>

void main()
{
    vec3 wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));

    v_normal    = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
    v_tangent   = normalize(mul(u_model[0], vec4(a_tangent.xyz, 0.0)).xyz);
    v_bitangent = cross(v_normal, v_tangent) * a_tangent.w;
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
}
