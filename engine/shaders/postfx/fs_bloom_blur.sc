/*
 * fs_bloom_blur.sc  Gaussian blur pass (horizontal or vertical).
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_blurDir;   /* xy=direction (1/w,0) or (0,1/h), zw=unused */

void main()
{
    vec2 dir = u_blurDir.xy;

    /* 9-tap Gaussian kernel (sigma ~= 4.0). */
    float weights[5];
    weights[0] = 0.2270270270;
    weights[1] = 0.1945945946;
    weights[2] = 0.1216216216;
    weights[3] = 0.0540540541;
    weights[4] = 0.0162162162;

    vec3 result = texture2D(s_texColor, v_texcoord0).rgb * weights[0];

    for (int i = 1; i < 5; i++) {
        vec2 offset = dir * float(i);
        result += texture2D(s_texColor, v_texcoord0 + offset).rgb * weights[i];
        result += texture2D(s_texColor, v_texcoord0 - offset).rgb * weights[i];
    }

    gl_FragColor = vec4(result, 1.0);
}
