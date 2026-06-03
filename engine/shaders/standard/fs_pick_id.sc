$input v_normal, v_texcoord0, v_worldpos

#include <bgfx_shader.sh>

uniform vec4 u_pickId;

void main()
{
    gl_FragColor = u_pickId;
}
