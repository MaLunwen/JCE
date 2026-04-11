/*
 * fs_bloom_extract.sc  Extract bright pixels above threshold.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_bloomParams;   /* x=threshold, y=intensity, z=unused, w=unused */

void main()
{
    vec3 color = texture2D(s_texColor, v_texcoord0).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float threshold = u_bloomParams.x;

    if (brightness > threshold)
        gl_FragColor = vec4(color * (brightness - threshold), 1.0);
    else
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
