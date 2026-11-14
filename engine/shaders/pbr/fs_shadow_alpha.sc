$input v_texcoord0
#include <bgfx_shader.sh>
#include "material_depth_alpha.sh"
void main()
{
    jce_material_depth_alpha(v_texcoord0);
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
