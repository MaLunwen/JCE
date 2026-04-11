/*
 * fs_fxaa.sc  Fast approximate anti-aliasing.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_fxaaParams;    /* x=spanMax, y=reduceMin, z=reduceMul, w=unused */
uniform vec4 u_texelSize;     /* xy=1/width,1/height, zw=width,height */

void main()
{
    vec2 texel = u_texelSize.xy;
    float spanMax   = u_fxaaParams.x;
    float reduceMin = u_fxaaParams.y;
    float reduceMul = u_fxaaParams.z;

    vec3 rgbNW = texture2D(s_texColor, v_texcoord0 + vec2(-1.0, -1.0) * texel).rgb;
    vec3 rgbNE = texture2D(s_texColor, v_texcoord0 + vec2( 1.0, -1.0) * texel).rgb;
    vec3 rgbSW = texture2D(s_texColor, v_texcoord0 + vec2(-1.0,  1.0) * texel).rgb;
    vec3 rgbSE = texture2D(s_texColor, v_texcoord0 + vec2( 1.0,  1.0) * texel).rgb;
    vec3 rgbM  = texture2D(s_texColor, v_texcoord0).rgb;

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM  = dot(rgbM,  luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * reduceMul,
                          reduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(spanMax, spanMax),
              max(vec2(-spanMax, -spanMax), dir * rcpDirMin)) * texel;

    vec3 rgbA = 0.5 * (
        texture2D(s_texColor, v_texcoord0 + dir * (1.0/3.0 - 0.5)).rgb +
        texture2D(s_texColor, v_texcoord0 + dir * (2.0/3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture2D(s_texColor, v_texcoord0 + dir * -0.5).rgb +
        texture2D(s_texColor, v_texcoord0 + dir *  0.5).rgb);

    float lumaB = dot(rgbB, luma);
    vec3 result = ((lumaB < lumaMin) || (lumaB > lumaMax)) ? rgbA : rgbB;

    gl_FragColor = vec4(result, 1.0);
}
