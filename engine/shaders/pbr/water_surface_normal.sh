#ifndef JCE_WATER_SURFACE_NORMAL_SH
#define JCE_WATER_SURFACE_NORMAL_SH

#include "water_surface_noise.sh"
#include "water_normal_detail.sh"

// Consumes the water draw's existing uniforms and stage-14 gradient. Run before
// dry coverage rejection so the same unwrapped derivative lanes reach both passes.
struct WaterSurfaceNormal
{
    vec3 normal;
    vec3 viewDirection;
    float facing; // Before the colour pass's two-sided flip; trace admits front faces.
    float iceRatio;
    float farDetail;
    float snowOnIce;
    float iceVeins;
    bool fft;
    bool detailedOcean;
    bool fftIce;
};

vec4 water_fft_gradient(vec2 uv, bool detailed, float relativeRange)
{
    // Derivatives are taken on the unwrapped coordinate. Mix the original
    // level with the filtered slope only while the cascade is still resolved.
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    vec4 filtered = texture2DGrad(s_water_gradient, uv, dx, dy);
    BRANCH if (detailed) {
        float footprint = max(length(dx), length(dy));
        float raw = (1.0 - smoothstep(0.010, 0.028, footprint)) *
                    (1.0 - smoothstep(0.60, 1.45, relativeRange));
        BRANCH if (raw > 0.0)
            filtered = mix(filtered, texture2DLod(s_water_gradient, uv, 0.0), raw);
    }
    return filtered;
}

WaterSurfaceNormal water_surface_normal(vec3 vertexNormal, vec3 worldPosition,
                                        vec3 referenceAndBlend, vec2 rippleSlope,
                                        float shoreDepth)
{
    vec3 N = vertexNormal;
    float iceRatio = clamp(u_water_shading.w, 0.0, 1.0);
    bool fft = u_water_mode.x > 0.5 && u_water_mode.x < 1.5;
    // The admitted ocean mesh selects the surface look. The resident gradient
    // only selects how the base FFT normal is reconstructed.
    bool detailedOcean = fft && u_water_params.w > 0.0 &&
                         u_water_absorb.w < 0.5 && u_cameraPos.y >= u_water_params.y;
    float fftRetention = 1.0 - referenceAndBlend.z;
    float patchSize = max(u_water_mode.y, 0.001);
    float eyeRange = detailedOcean ? length(u_cameraPos.xyz - worldPosition) : 0.0;
    float farDetail = 1.0 - smoothstep(patchSize * 1.5, patchSize * 4.8, eyeRange);
    if (fft && u_water_mode.z > 0.5)
    {
        vec2 slope = water_fft_gradient(referenceAndBlend.xy / patchSize, detailedOcean, eyeRange / patchSize).rg;
        if (detailedOcean)
            slope *= mix(0.30, 1.0, farDetail);
        if (u_water_mode.w > 0.0) {
            float secondPatchSize = u_water_mode.w;
            vec2 secondSlope = water_fft_gradient(referenceAndBlend.xy / secondPatchSize, detailedOcean,
                                                 eyeRange / secondPatchSize).ba;
            if (detailedOcean)
                secondSlope *= 1.0 - smoothstep(secondPatchSize * 1.65, secondPatchSize * 4.5, eyeRange);
            slope += secondSlope;
        }
        if (detailedOcean) {
            // The shallow solver supplies the broad surface orientation.
            // Preserve only a fraction of the short-wave slope in that domain.
            float shallowSlope = mix(0.12, 0.32, smoothstep(0.06, 0.48, shoreDepth));
            fftRetention += referenceAndBlend.z * shallowSlope;
        }
        slope *= fftRetention;
        N = vec3(-slope.x + rippleSlope.x, 1.0, -slope.y + rippleSlope.y);
    }
    BRANCH if (detailedOcean && iceRatio < 1.0)
    {
        BRANCH if (u_water_normal_detail.x > 0.0 && u_water_normal_detail.y > 0.0)
        {
            vec2 slope = water_normal_detail_slope(referenceAndBlend.xy);
            float retention = (1.0 - iceRatio) * fftRetention;
            N.xz -= slope * (retention * N.y);
        }
        else
        {
            // Capillary detail rides the displaced surface's reference XZ.
            // Suppress the analytic slope when a pixel spans its lattice.
            vec2 xz = referenceAndBlend.xy;
            vec2 microUv = vec2(0.8 * xz.x + 0.6 * xz.y,
                               -0.6 * xz.x + 0.8 * xz.y) * 2.1;
            float microFade = water_detail_weight(microUv);
            BRANCH if (microFade > 0.0)
            {
                vec2 grain = water_noise_gradient(microUv).yz;
                vec2 microSlope = vec2(0.8 * grain.x - 0.6 * grain.y,
                                       0.6 * grain.x + 0.8 * grain.y);
                // For either base normal, N.xz / N.y represents the signed slope.
                float strength = 0.14 * microFade * (1.0 - iceRatio) * fftRetention;
                N.xz -= microSlope * (strength * N.y);
            }
        }
    }
    N = normalize(N);
    // Sparse, world-anchored raindrop rings perturb the existing wave normal.
    // A ring is contained in its own cell and fades before its phase resets.
    BRANCH if (u_water_time.y > 0.0) {
        vec2 rainUv = worldPosition.xz * 3.5;
        float footprint = max(length(dFdx(rainUv)), length(dFdy(rainUv)));
        float visible = 1.0 - smoothstep(0.05, 0.21, footprint);
        if (visible > 0.0) {
            vec2 cell = floor(rainUv);
            float seed = water_hash12(cell + vec2(13.7, 47.1));
            if (seed < clamp(u_water_time.y, 0.0, 1.0)) {
                vec2 center = vec2(water_hash12(cell), water_hash12(cell + 37.0));
                vec2 delta = fract(rainUv) - mix(vec2_splat(0.35), vec2_splat(0.65), center);
                float radius = length(delta);
                float phase = fract(u_water_time.x * (1.03 + 0.45 * seed) + center.x);
                float difference = radius - (0.04 + 0.21 * phase);
                float ring = exp2(-difference * difference * 2200.0);
                float ramp = smoothstep(0.0, 0.12, phase)
                           * (1.0 - smoothstep(0.45, 1.0, phase));
                float slope = sin(difference * 109.0) * ring * ramp * visible * 0.08;
                vec2 rainSlope = delta * (slope / max(radius, 0.001));
                N = normalize(N + vec3(rainSlope.x, 0.0, rainSlope.y));
            }
        }
    }
    // Water surface always faces up toward the viewer above it; flip a
    // back-facing normal so under-the-surface / steep views stay lit.
    vec3 V = normalize(u_cameraPos.xyz - worldPosition);
    bool fftIce = iceRatio > 0.001 && u_water_mode.x > 0.5 && u_water_mode.x < 1.5;
    float snowOnIce = 0.0;
    float iceVeins = 0.0;
    BRANCH if (fftIce)
    {
        // Undisplaced world XZ prevents ice and snow patterns from swimming
        // with the FFT. Use one refracted view ray and three distinct planes.
        vec2 iceXZ = referenceAndBlend.xy;
        float iceTop = smoothstep(0.0, 0.065, V.y);
        vec3 topView = normalize(vec3(V.x, max(V.y, 0.002), V.z));
        vec3 ray = refract(-topView, vec3(0.0, 1.0, 0.0), 1.0 / 1.31);
        vec2 parallax = ray.xz / max(-ray.y, 0.60) * iceTop;
        float nearVein = water_ice_vein((iceXZ + parallax * 0.06) * 2.60);
        float midVein  = water_ice_vein((iceXZ + parallax * 0.24) * 1.85 + vec2(9.4, 3.1));
        float farVein  = water_ice_vein((iceXZ + parallax * 0.54) * 1.25 + vec2(-5.8, 18.3));
        iceVeins = dot(vec3(nearVein, midVein, farVein), vec3(0.46, 0.34, 0.20));

        vec2 patchUv = iceXZ * 0.47 + vec2(24.7, -9.2);
        vec2 grainUv = iceXZ * 4.3 + vec2(8.7, 16.1);
        float patchDetail = water_detail_weight(patchUv);
        float grainDetail = water_detail_weight(grainUv);
        float snow = clamp(u_weatherSurface.y, 0.0, 1.0);
        vec3 grain = vec3(0.5, 0.0, 0.0);
        BRANCH if (grainDetail > 0.0) grain = water_noise_gradient(grainUv);
        BRANCH if (snow > 0.0)
        {
            float snowPattern = 0.5;
            BRANCH if (patchDetail > 0.0)
                snowPattern = mix(0.5, water_vnoise2(patchUv), patchDetail);
            snowPattern += (grain.x - 0.5) * grainDetail * 0.22;
            float edge = mix(0.81, 0.37, snow);
            snowOnIce = iceTop * smoothstep(0.0, 0.55, snow) *
                smoothstep(edge - 0.09, edge + 0.09, snowPattern);
        }

        N = mix(N, vec3(0.0, 1.0, 0.0), iceRatio * 0.56);
        float micro = iceRatio * iceTop * grainDetail * mix(0.055, 0.13, snowOnIce);
        N = normalize(N + vec3(-grain.y * micro, 0.0, -grain.z * micro));
    }
    float facing = dot(N, V);
    if (facing < 0.0) N = -N;
    WaterSurfaceNormal result;
    result.normal = N;
    result.viewDirection = V;
    result.facing = facing;
    result.iceRatio = iceRatio;
    result.farDetail = farDetail;
    result.snowOnIce = snowOnIce;
    result.iceVeins = iceVeins;
    result.fft = fft;
    result.detailedOcean = detailedOcean;
    result.fftIce = fftIce;
    return result;
}

#endif
