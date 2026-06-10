/*
 * Static-geometry pick vertex shader. Position only, NO output varyings, so it
 * links with fs_pick (no inputs). The pick pass sets u_model[0] per draw
 * (bgfx_set_transform), exactly like the static render path.
 */
$input a_position

#include <bgfx_shader.sh>

void main()
{
    vec3 wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));
}
