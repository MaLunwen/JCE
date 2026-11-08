$input a_position, a_texcoord0, a_texcoord1, a_color0
$output v_texcoord0, v_pcolor

#include <bgfx_shader.sh>

uniform vec4 u_particle_misc;

void main()
{
    vec3 center = a_position;
    float size = a_texcoord1.w;
    vec2 quad_corner = a_texcoord0 - vec2(0.5, 0.5);
#define JCE_PARTICLE_STRETCH_VECTOR a_texcoord1.xyz
#define JCE_PARTICLE_CORNER quad_corner
#include "particle_billboard.sh"
#undef JCE_PARTICLE_CORNER
#undef JCE_PARTICLE_STRETCH_VECTOR
    v_texcoord0 = a_texcoord0;
    v_pcolor = a_color0;
}
