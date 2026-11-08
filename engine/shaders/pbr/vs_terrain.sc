$input a_position, a_normal, a_tangent, a_texcoord0
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

#include <bgfx_shader.sh>
#include "terrain_snow_height.sh"

void main()
{
    vec3 wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    float base_y = wpos.y;
    v_normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
    wpos = terrain_snow_displace(wpos, v_normal.y);
    vec4 viewPos = mul(u_view, vec4(wpos, 1.0));
    gl_Position = mul(u_proj, viewPos);

    v_tangent   = normalize(mul(u_model[0], vec4(a_tangent.xyz, 0.0)).xyz);
    v_bitangent = cross(v_normal, v_tangent) * a_tangent.w;
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
    v_localpos  = vec3(a_position.x, base_y, a_position.z);
    v_viewdepth = -viewPos.z;
}
