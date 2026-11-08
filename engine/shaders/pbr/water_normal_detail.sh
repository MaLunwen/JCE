#ifndef JCE_WATER_NORMAL_DETAIL_SH
#define JCE_WATER_NORMAL_DETAIL_SH

// Explicit world-space transport; phase is borrowed from the evaluated FFT.
// The two independent rotations/offsets preserve the single-layer slope energy.
vec2 water_normal_detail_band(vec2 uv, vec2 rotation, float detail)
{
    BRANCH if (detail <= 0.0) return vec2_splat(0.0);
    vec2 gradient = water_noise_gradient(uv).yz;
    return vec2(rotation.x * gradient.x - rotation.y * gradient.y,
                rotation.y * gradient.x + rotation.x * gradient.y) * detail;
}

vec2 water_normal_detail_slope(vec2 referenceXZ)
{
    // Velocities are metres per spectral phase second, independent of frequency.
    vec2 lowXZ = referenceXZ - vec2(0.055, 0.020) * u_water_normal_detail.w;
    vec2 highXZ = referenceXZ - vec2(-0.025, 0.018) * u_water_normal_detail.w;
    vec2 lowRotation = vec2(0.8, 0.6);
    vec2 highRotation = vec2(5.0 / 13.0, 12.0 / 13.0);
    vec2 lowUv = vec2(lowRotation.x * lowXZ.x + lowRotation.y * lowXZ.y,
                    -lowRotation.y * lowXZ.x + lowRotation.x * lowXZ.y) * u_water_normal_detail.x;
    vec2 highUv = (vec2(highRotation.x * highXZ.x + highRotation.y * highXZ.y,
                      -highRotation.y * highXZ.x + highRotation.x * highXZ.y) +
                   vec2(17.13, -8.57)) * u_water_normal_detail.y;
    // Both derivative footprints precede per-pixel detail rejection. Each band
    // fades independently; an unresolved fine band must not erase the low band.
    float lowDetail = water_detail_weight(lowUv);
    float highDetail = water_detail_weight(highUv);
    vec2 low = water_normal_detail_band(lowUv, lowRotation, lowDetail);
    vec2 high = water_normal_detail_band(highUv, highRotation, highDetail);
    // 0.8^2 + 0.6^2 = 1: independent resolved bands retain the old RMS energy.
    return (0.8 * low + 0.6 * high) * u_water_normal_detail.z;
}

#endif
