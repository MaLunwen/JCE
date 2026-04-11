/*
 * fs_grayscale.sc  Desaturation effect.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
    vec3 color = texture2D(s_texColor, v_texcoord0).rgb;
    float gray = dot(color, vec3(0.2126, 0.7152, 0.0722));
    gl_FragColor = vec4(vec3_splat(gray), 1.0);
}
