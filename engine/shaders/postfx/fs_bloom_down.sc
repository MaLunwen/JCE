/* fs_bloom_down.sc  CoD/Jimenez 13-tap dual-filter downsample.
 *
 * Mip0 uses a Karis luma-weighted average of the 5 overlapping 2x2 quads
 * to suppress fireflies (bright single-pixel lights that would streak).
 * Subsequent mips use a simpler weighted box to maintain smoothness.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_texelSize;   /* xy = 1/srcW, 1/srcH */
uniform vec4 u_bloomParams; /* w = 1.0 on mip0 (Karis avg), else 0 */

vec3 tap(vec2 uv){ return texture2D(s_texColor, uv).rgb; }
float karisWeight(vec3 c){ return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722))); }

void main()
{
    vec2 t  = u_texelSize.xy;
    vec2 uv = v_texcoord0;

    vec3 a = tap(uv + t*vec2(-2.0,  2.0));
    vec3 b = tap(uv + t*vec2( 0.0,  2.0));
    vec3 c = tap(uv + t*vec2( 2.0,  2.0));
    vec3 d = tap(uv + t*vec2(-2.0,  0.0));
    vec3 e = tap(uv);
    vec3 f = tap(uv + t*vec2( 2.0,  0.0));
    vec3 g = tap(uv + t*vec2(-2.0, -2.0));
    vec3 h = tap(uv + t*vec2( 0.0, -2.0));
    vec3 i = tap(uv + t*vec2( 2.0, -2.0));
    vec3 j = tap(uv + t*vec2(-1.0,  1.0));
    vec3 k = tap(uv + t*vec2( 1.0,  1.0));
    vec3 l = tap(uv + t*vec2(-1.0, -1.0));
    vec3 m = tap(uv + t*vec2( 1.0, -1.0));

    vec3 result;
    if (u_bloomParams.w > 0.5)
    {
        /* Mip0: Karis luma-weighted average of 5 overlapping 2x2 quads.
         * Kills fireflies by down-weighting super-bright pixels. */
        vec3 g0 = (j + k + l + m) * 0.25;
        vec3 g1 = (a + b + d + e) * 0.25;
        vec3 g2 = (b + c + e + f) * 0.25;
        vec3 g3 = (d + e + g + h) * 0.25;
        vec3 g4 = (e + f + h + i) * 0.25;
        float w0 = karisWeight(g0);
        float w1 = karisWeight(g1);
        float w2 = karisWeight(g2);
        float w3 = karisWeight(g3);
        float w4 = karisWeight(g4);
        result = (g0*w0 + g1*w1 + g2*w2 + g3*w3 + g4*w4) / (w0 + w1 + w2 + w3 + w4);
    }
    else
    {
        /* Subsequent mips: standard CoD weighted box. */
        result  = e * 0.125;
        result += (a + c + g + i) * 0.03125;
        result += (b + d + f + h) * 0.0625;
        result += (j + k + l + m) * 0.125;
    }

    gl_FragColor = vec4(result, 1.0);
}
