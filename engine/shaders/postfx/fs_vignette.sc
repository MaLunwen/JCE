/*
 * fs_vignette.sc  Screen-edge darkening effect.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_vignetteParams;   /* x=intensity, y=smoothness, z=unused, w=unused */

void main()
{
    vec3 color = texture2D(s_texColor, v_texcoord0).rgb;

    float intensity  = u_vignetteParams.x;
    float smoothness = u_vignetteParams.y;

    vec2 uv = v_texcoord0 * 2.0 - 1.0;
    float dist = dot(uv, uv);
    float vignette = 1.0 - smoothstep(1.0 - smoothness, 1.0, dist * intensity);

    gl_FragColor = vec4(color * vignette, 1.0);
}
