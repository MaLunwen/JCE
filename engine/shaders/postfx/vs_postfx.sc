/*
 * vs_postfx.sc  Full-screen triangle vertex shader for post-processing.
 */

$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_texcoord0 = a_texcoord0;
#if BGFX_SHADER_LANGUAGE_GLSL
    /* OpenGL texture coordinates are bottom-left based; flip V so each
       post-process pass preserves orientation rather than toggling it. */
    v_texcoord0.y = 1.0 - v_texcoord0.y;
#endif
}
