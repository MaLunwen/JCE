/*
 * fs_chromatic.sc  Chromatic aberration effect.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_chromaticParams;   /* x=strength, y=unused, z=unused, w=unused */

void main()
{
    float strength = u_chromaticParams.x;

    vec2 dir = v_texcoord0 - vec2(0.5, 0.5);
    vec2 offset = dir * strength;

    float r = texture2D(s_texColor, v_texcoord0 + offset).r;
    float g = texture2D(s_texColor, v_texcoord0).g;
    float b = texture2D(s_texColor, v_texcoord0 - offset).b;

    gl_FragColor = vec4(r, g, b, 1.0);
}
