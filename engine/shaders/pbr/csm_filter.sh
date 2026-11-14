/* Fixed, balanced disk PCF shared by mesh, terrain and water receivers.
 *
 * Point-sampled square grids produced aligned steps.  Rotating a grid with a
 * high-frequency world-position hash replaced those steps with spatial noise
 * that changes as moving screen pixels sample the surface.  These symmetric
 * rings need neither a hash nor sin/cos.  Every tap has an opposite, and X/Y
 * second moments agree: the removed noise is not traded for a one-sided blur.
 *
 * Keep the 9/25 depth-fetch budgets and essentially the same second moment as
 * the old 3x3/5x5 grids (2/3 and 2.0032 vs 2 texels squared per axis).  Radius,
 * PCSS, bias, cascade selection and dynamic-atlas policy stay with the caller.
 * Explicit constants keep GLSL 1.20 free of variable array/loop indexing.
 */
#ifndef JCE_CSM_FILTER_SH
#define JCE_CSM_FILTER_SH

float csm_inverse_map_size(float supplied)
{
    // Valid 4096/8192 maps must use their actual, smaller texel footprint.
    return supplied > 0.0 ? supplied : (1.0 / 2048.0);
}

/* Dynamic-atlas PCF: reconstruct comparison coverage continuously inside each
 * texel, using the same nine POINT depth reads as the former equal-weight 3x3.
 * Filter comparison results, never interpolated depth.  A separable quadratic
 * B-spline has nonnegative unit-sum weights; at an anchor change the departing
 * tap has zero weight and the other weights agree with the next anchor.
 *
 * This deliberately has a more concentrated footprint than the former box:
 * variance 1/4 texel^2 per axis about the receiver, versus the box's 2/3 about
 * its snapped centre.  It removes phase plateaus without enlarging the atlas,
 * increasing the fetch count, or hiding low resolution behind a wider blur.
 * Atlas addressing, hard tier, bias and min-combination remain caller policy.
 */
float csm_dynamic_pcf(sampler2D shadow_map, vec2 atlas_uv, float receiver_z,
                      float depth_bias, vec2 texel, vec2 lo, vec2 hi)
{
    vec2 position = atlas_uv / texel;
    vec2 anchor = floor(position);
    vec2 phase = position - anchor - 0.5;
    vec2 left = 0.5 - phase;
    vec2 right = 0.5 + phase;
    vec2 w0 = 0.5 * left * left;
    vec2 w1 = 0.75 - phase * phase;
    vec2 w2 = 0.5 * right * right;
    vec2 centre = (anchor + 0.5) * texel;
    float receiver = receiver_z - depth_bias;
    float sum = 0.0;
#define JCE_CSM_DYNAMIC_TAP(x, y, weight) \
    sum += (weight) * ((receiver > texture2D(shadow_map, \
        clamp(centre + vec2(x, y) * texel, lo, hi)).r) ? 0.0 : 1.0)
    JCE_CSM_DYNAMIC_TAP(-1.0, -1.0, w0.x * w0.y);
    JCE_CSM_DYNAMIC_TAP( 0.0, -1.0, w1.x * w0.y);
    JCE_CSM_DYNAMIC_TAP( 1.0, -1.0, w2.x * w0.y);
    JCE_CSM_DYNAMIC_TAP(-1.0,  0.0, w0.x * w1.y);
    JCE_CSM_DYNAMIC_TAP( 0.0,  0.0, w1.x * w1.y);
    JCE_CSM_DYNAMIC_TAP( 1.0,  0.0, w2.x * w1.y);
    JCE_CSM_DYNAMIC_TAP(-1.0,  1.0, w0.x * w2.y);
    JCE_CSM_DYNAMIC_TAP( 0.0,  1.0, w1.x * w2.y);
    JCE_CSM_DYNAMIC_TAP( 1.0,  1.0, w2.x * w2.y);
#undef JCE_CSM_DYNAMIC_TAP
    return sum;
}

float csm_pcf_filter(sampler2D shadow_map, vec2 csm_uv, float csm_z,
                     float depth_bias, vec2 texel, float filter_radius)
{
    vec2 scale = texel * filter_radius;
    float receiver = csm_z - depth_bias;
    float sum = 0.0;
#define JCE_CSM_TAP(x, y) \
    sum += (receiver > texture2D(shadow_map, csm_uv + vec2(x, y) * scale).r) ? 0.0 : 1.0

    if (u_shadowQuality.x < 1.5)
    {
        // Tier 1: centre plus eight balanced directions, nine fetches.
        JCE_CSM_TAP( 0.0000000,  0.0000000);
        JCE_CSM_TAP( 1.1315167,  0.4686896);
        JCE_CSM_TAP( 0.4686896,  1.1315167);
        JCE_CSM_TAP(-0.4686896,  1.1315167);
        JCE_CSM_TAP(-1.1315167,  0.4686896);
        JCE_CSM_TAP(-1.1315167, -0.4686896);
        JCE_CSM_TAP(-0.4686896, -1.1315167);
        JCE_CSM_TAP( 0.4686896, -1.1315167);
        JCE_CSM_TAP( 1.1315167, -0.4686896);
        return sum * (1.0 / 9.0);
    }

    // Tier 2: centre, eight inner and sixteen staggered outer taps.
    JCE_CSM_TAP( 0.0000000,  0.0000000);
    JCE_CSM_TAP( 0.9238795,  0.3826834);
    JCE_CSM_TAP( 0.3826834,  0.9238795);
    JCE_CSM_TAP(-0.3826834,  0.9238795);
    JCE_CSM_TAP(-0.9238795,  0.3826834);
    JCE_CSM_TAP(-0.9238795, -0.3826834);
    JCE_CSM_TAP(-0.3826834, -0.9238795);
    JCE_CSM_TAP( 0.3826834, -0.9238795);
    JCE_CSM_TAP( 0.9238795, -0.3826834);
    JCE_CSM_TAP( 2.3538847,  0.4682168);
    JCE_CSM_TAP( 1.9955271,  1.3333686);
    JCE_CSM_TAP( 1.3333686,  1.9955271);
    JCE_CSM_TAP( 0.4682168,  2.3538847);
    JCE_CSM_TAP(-0.4682168,  2.3538847);
    JCE_CSM_TAP(-1.3333686,  1.9955271);
    JCE_CSM_TAP(-1.9955271,  1.3333686);
    JCE_CSM_TAP(-2.3538847,  0.4682168);
    JCE_CSM_TAP(-2.3538847, -0.4682168);
    JCE_CSM_TAP(-1.9955271, -1.3333686);
    JCE_CSM_TAP(-1.3333686, -1.9955271);
    JCE_CSM_TAP(-0.4682168, -2.3538847);
    JCE_CSM_TAP( 0.4682168, -2.3538847);
    JCE_CSM_TAP( 1.3333686, -1.9955271);
    JCE_CSM_TAP( 1.9955271, -1.3333686);
    JCE_CSM_TAP( 2.3538847, -0.4682168);
#undef JCE_CSM_TAP
    return sum * (1.0 / 25.0);
}

#endif /* JCE_CSM_FILTER_SH */
