/*
 * fs_tonemap.sc  ACES tone mapping + gamma correction.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_tonemapParams;   /* x=exposure, y=gamma, z=unused, w=unused */

vec3 aces_film(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 hdr = texture2D(s_texColor, v_texcoord0).rgb;
    float exposure = u_tonemapParams.x;
    float gamma    = u_tonemapParams.y;

    vec3 mapped = aces_film(hdr * exposure);
    vec3 corrected = pow(mapped, vec3_splat(1.0 / gamma));

    gl_FragColor = vec4(corrected, 1.0);
}
