/* fs_bloom_up.sc  9-tap tent additive upsample for the bloom pyramid.
 *
 * Each upsample step blends a lower-resolution mip additively into the
 * higher-resolution target, building up a wide, soft glow.  The tent
 * filter (1/16 * [1 2 1 / 2 4 2 / 1 2 1]) is separable but implemented
 * as a single 9-tap gather for clarity; the cost is acceptable given the
 * low resolution of the mip levels being upsampled.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);   /* lower-resolution mip being upsampled */
uniform vec4 u_texelSize;   /* xy = 1/dstW, 1/dstH (tent radius in dst texels) */

void main()
{
    vec2 t  = u_texelSize.xy;
    vec2 uv = v_texcoord0;

    vec3 s;
    s  = texture2D(s_texColor, uv + t*vec2(-1.0,  1.0)).rgb;
    s += texture2D(s_texColor, uv + t*vec2( 0.0,  1.0)).rgb * 2.0;
    s += texture2D(s_texColor, uv + t*vec2( 1.0,  1.0)).rgb;
    s += texture2D(s_texColor, uv + t*vec2(-1.0,  0.0)).rgb * 2.0;
    s += texture2D(s_texColor, uv).rgb * 4.0;
    s += texture2D(s_texColor, uv + t*vec2( 1.0,  0.0)).rgb * 2.0;
    s += texture2D(s_texColor, uv + t*vec2(-1.0, -1.0)).rgb;
    s += texture2D(s_texColor, uv + t*vec2( 0.0, -1.0)).rgb * 2.0;
    s += texture2D(s_texColor, uv + t*vec2( 1.0, -1.0)).rgb;

    gl_FragColor = vec4(s * (1.0 / 16.0), 1.0);
}
