$input a_position, a_normal
$output v_texcoord0

#include <bgfx_shader.sh>
#include "shadow_pancake.sh"
#include "terrain_snow_height.sh"

void main()
{
    vec3 wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    vec3 normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
    wpos = terrain_snow_displace(wpos, normal.y);
    gl_Position = jce_shadow_pancake(mul(u_viewProj, vec4(wpos, 1.0)));
    v_texcoord0 = vec2(0.0, 0.0);
}
