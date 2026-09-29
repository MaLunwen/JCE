$input v_texcoord0

#include <bgfx_shader.sh>
#include "fixture_math.sh"

void main()
{
    gl_FragColor = vec4(fixture_colour(v_texcoord0), 1.0);
}
