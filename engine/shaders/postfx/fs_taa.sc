/*
 * fs_taa.sc  Temporal Anti-Aliasing resolve.
 *
 * Inputs:
 *   s_texColor   : current frame's jittered colour
 *   s_texHistory : previous frame's resolved (un-jittered) history
 *   s_texMotion  : motion vectors (xy) in NDC delta — current → previous
 *
 * Algorithm (Karis 2014 / Salvi 2016 distillation):
 *   1) Reproject the previous-frame UV using the motion vector.
 *   2) Sample a 3×3 neighbourhood of the current frame and build a
 *      luminance bounding box (min/max + variance).
 *   3) AABB-clamp the history sample inside that box to kill ghosting
 *      from disocclusion.
 *   4) Blend with feedback weight; lower the weight when the history
 *      lookup left the screen.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor,   0);
SAMPLER2D(s_texHistory, 1);
SAMPLER2D(s_texMotion,  2);

/* xy = 1/w, 1/h     zw = unused */
uniform vec4 u_texelSize;
/* x = feedback (0.85-0.97 typical)
 * y = luma_clamp_scale (1.0 default; 0 = no clamp)
 * z = motion_clamp_strength (1.0 default)
 * w = unused */
uniform vec4 u_taaParams;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main()
{
    vec2 texel = u_texelSize.xy;

    /* Current sample. */
    vec4 cur = texture2D(s_texColor, v_texcoord0);

    /* Reproject. Motion vectors stored in [-1,1] NDC delta encoded in
     * [0,1] : decoded as mv * 2 - 1.  Subtract from current UV → prev UV. */
    vec2 mv = texture2D(s_texMotion, v_texcoord0).xy * 2.0 - 1.0;
    vec2 prev_uv = v_texcoord0 - mv * 0.5;  /* /2 because NDC = 2*UV-1 */

    vec4 hist = texture2D(s_texHistory, prev_uv);

    /* 3x3 neighbourhood luma bbox (Salvi). */
    vec3 c00 = texture2D(s_texColor, v_texcoord0 + vec2(-texel.x, -texel.y)).rgb;
    vec3 c10 = texture2D(s_texColor, v_texcoord0 + vec2(0.0,      -texel.y)).rgb;
    vec3 c20 = texture2D(s_texColor, v_texcoord0 + vec2( texel.x, -texel.y)).rgb;
    vec3 c01 = texture2D(s_texColor, v_texcoord0 + vec2(-texel.x,  0.0)).rgb;
    vec3 c11 = cur.rgb;
    vec3 c21 = texture2D(s_texColor, v_texcoord0 + vec2( texel.x,  0.0)).rgb;
    vec3 c02 = texture2D(s_texColor, v_texcoord0 + vec2(-texel.x,  texel.y)).rgb;
    vec3 c12 = texture2D(s_texColor, v_texcoord0 + vec2(0.0,       texel.y)).rgb;
    vec3 c22 = texture2D(s_texColor, v_texcoord0 + vec2( texel.x,  texel.y)).rgb;

    vec3 mn = min(min(min(min(c00, c10), min(c20, c01)),
                      min(min(c11, c21), min(c02, c12))), c22);
    vec3 mx = max(max(max(max(c00, c10), max(c20, c01)),
                      max(max(c11, c21), max(c02, c12))), c22);

    /* Variance-based softening of the box (Karis): expand by σ. */
    vec3 mean = (c00 + c10 + c20 + c01 + c11 + c21 + c02 + c12 + c22) / 9.0;
    vec3 var  = (c00*c00 + c10*c10 + c20*c20 + c01*c01 + c11*c11 +
                 c21*c21 + c02*c02 + c12*c12 + c22*c22) / 9.0 - mean*mean;
    vec3 sigma = sqrt(max(var, vec3_splat(0.0))) * u_taaParams.y;
    mn = max(mn, mean - sigma);
    mx = min(mx, mean + sigma);

    /* Clip history to neighbourhood bbox (component-wise). */
    hist.rgb = clamp(hist.rgb, mn, mx);

    /* History invalid when reprojected UV left the screen. */
    float onscreen = step(0.0, prev_uv.x) * step(prev_uv.x, 1.0) *
                     step(0.0, prev_uv.y) * step(prev_uv.y, 1.0);

    float feedback = u_taaParams.x * onscreen;

    /* Lower feedback in high-motion regions to reduce blur. */
    float speed = length(mv);
    feedback *= mix(1.0, 0.7, clamp(speed * u_taaParams.z, 0.0, 1.0));

    vec3 outc = mix(cur.rgb, hist.rgb, feedback);
    gl_FragColor = vec4(outc, cur.a);
}
