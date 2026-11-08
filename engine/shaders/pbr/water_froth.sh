#ifndef JCE_WATER_FROTH_SH
#define JCE_WATER_FROTH_SH
// Smooth warped fBm isocontours form irregular pores without cell walls.
// Three value-noise taps replace the nine-neighbour cellular search.
float water_froth_signal(vec2 uv)
{
    float warp = water_vnoise2(uv * 0.37 + vec2(17.2, 4.7)) - 0.5;
    vec2 q = vec2(0.8*uv.x + 0.6*uv.y, -0.6*uv.x + 0.8*uv.y) +
             vec2(warp, warp*warp - 0.08) * 1.6;
    vec2 r = vec2(0.384615*q.x - 0.923077*q.y, 0.923077*q.x + 0.384615*q.y);
    float n = 0.72 * water_vnoise2(q) +
              0.28 * water_vnoise2(r * 2.13 + vec2(4.2, 21.8));
    return 1.0 - abs(2.0*n - 1.0);
}

float water_froth_quantile(float probability)
{
    // Monotone empirical inverse CDF of this exact band.
    if (probability < 0.02000000) return mix(0.031370997, 0.299301197, clamp((probability - 0.00000000) / 0.02000000, 0.0, 1.0));
    if (probability < 0.05000000) return mix(0.299301197, 0.379728337, clamp((probability - 0.02000000) / 0.03000000, 0.0, 1.0));
    if (probability < 0.10000000) return mix(0.379728337, 0.458118868, clamp((probability - 0.05000000) / 0.05000000, 0.0, 1.0));
    if (probability < 0.20000000) return mix(0.458118868, 0.559991598, clamp((probability - 0.10000000) / 0.10000000, 0.0, 1.0));
    if (probability < 0.35000000) return mix(0.559991598, 0.668342713, clamp((probability - 0.20000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.50000000) return mix(0.668342713, 0.756483287, clamp((probability - 0.35000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.65000000) return mix(0.756483287, 0.834082386, clamp((probability - 0.50000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.80000000) return mix(0.834082386, 0.906774116, clamp((probability - 0.65000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.90000000) return mix(0.906774116, 0.953680837, clamp((probability - 0.80000000) / 0.10000000, 0.0, 1.0));
    if (probability < 0.95000000) return mix(0.953680837, 0.976843831, clamp((probability - 0.90000000) / 0.05000000, 0.0, 1.0));
    if (probability < 0.98000000) return mix(0.976843831, 0.990634580, clamp((probability - 0.95000000) / 0.03000000, 0.0, 1.0));
    if (probability < 1.00000000) return mix(0.990634580, 0.999999404, clamp((probability - 0.98000000) / 0.02000000, 0.0, 1.0));
    return 0.999999404;
}

float water_froth_patch_signal(vec2 uv)
{
    vec2 r = vec2(0.384615*uv.x - 0.923077*uv.y, 0.923077*uv.x + 0.384615*uv.y);
    return 0.75 * water_vnoise2(uv) +
           0.25 * water_vnoise2(r * 2.17 + vec2(13.7, 28.3));
}

float water_froth_patch_quantile(float probability)
{
    // Monotone empirical inverse CDF of this exact band.
    if (probability < 0.02000000) return mix(0.017985249, 0.171549878, clamp((probability - 0.00000000) / 0.02000000, 0.0, 1.0));
    if (probability < 0.05000000) return mix(0.171549878, 0.221355589, clamp((probability - 0.02000000) / 0.03000000, 0.0, 1.0));
    if (probability < 0.10000000) return mix(0.221355589, 0.271945062, clamp((probability - 0.05000000) / 0.05000000, 0.0, 1.0));
    if (probability < 0.20000000) return mix(0.271945062, 0.341527516, clamp((probability - 0.10000000) / 0.10000000, 0.0, 1.0));
    if (probability < 0.35000000) return mix(0.341527516, 0.422482452, clamp((probability - 0.20000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.50000000) return mix(0.422482452, 0.495338202, clamp((probability - 0.35000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.65000000) return mix(0.495338202, 0.569154862, clamp((probability - 0.50000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.80000000) return mix(0.569154862, 0.652315903, clamp((probability - 0.65000000) / 0.15000000, 0.0, 1.0));
    if (probability < 0.90000000) return mix(0.652315903, 0.724644083, clamp((probability - 0.80000000) / 0.10000000, 0.0, 1.0));
    if (probability < 0.95000000) return mix(0.724644083, 0.776195779, clamp((probability - 0.90000000) / 0.05000000, 0.0, 1.0));
    if (probability < 0.98000000) return mix(0.776195779, 0.826252681, clamp((probability - 0.95000000) / 0.03000000, 0.0, 1.0));
    if (probability < 1.00000000) return mix(0.826252681, 0.983354568, clamp((probability - 0.98000000) / 0.02000000, 0.0, 1.0));
    return 0.983354568;
}

float water_froth_band(vec2 uv, float footprint, float density, bool patch)
{
    float visible = 1.0 - smoothstep(0.12, 0.60, footprint);
    if (visible <= 0.0) return density;
    float signal = patch ? water_froth_patch_signal(uv) : water_froth_signal(uv);
    float threshold = patch ? water_froth_patch_quantile(1.0-density) :
                              water_froth_quantile(1.0-density);
    float aa = 0.008 + min(footprint*0.35, 0.09);
    threshold = mix(1.0 + aa, threshold, smoothstep(0.0, 0.01, density));
    threshold = mix(-aa, threshold, smoothstep(0.0, 0.01, 1.0-density));
    float coverage = smoothstep(threshold-aa, threshold+aa, signal);
    return mix(density, coverage, visible);
}

float water_froth_coverage_flow(vec2 positionM, float authoredScale, float footprintM,
    float density, vec2 velocity)
{
    density = clamp(density, 0.0, 1.0);
    if (density <= 0.0 || density >= 1.0) return density;
    float cellM = 0.09 / max(authoredScale, 0.20);
    float splitCoverage = sqrt(density);
    // Each independently offset band occupies sqrt(d). Their product keeps
    // the physical density while placing porous foam only inside fragments.
    // Filter each scale independently: far fine pores retain macro patches.
    float macro = water_froth_band(positionM / (cellM * 8.0) + vec2(41.7, -23.9),
                                    footprintM / (cellM * 8.0), splitCoverage, true);
    // footprintM is computed before divergence by the caller; no derivatives
    // or implicit texture LOD are evaluated inside either procedural band.
    BRANCH if (macro <= 0.0) return 0.0;
    float fine = water_froth_band(positionM / cellM, footprintM / cellM,
                                   splitCoverage, false);
    return fine * macro;
}

float water_froth_coverage(vec2 positionM, float authoredScale, float footprintM, float density)
{
    return water_froth_coverage_flow(positionM, authoredScale, footprintM, density, vec2_splat(0.0));
}

vec2 water_froth_flow_domain(vec2 positionM, vec2 velocity, float timeOffset)
{
    return positionM - velocity * timeOffset;
}

float water_shore_froth(vec2 positionM, float footprintM, float density,
    vec2 velocity, float timeS, float authoredScale)
{
    if (density <= 0.0) return 0.0;
    // Two bounded, offset phases: each resets only at zero weight and has
    // maximum weight at zero distortion. No velocity * absolute-time drift.
    float phase = fract(timeS * 0.25 + water_vnoise2(positionM * 0.04) * 0.25);
    float other = fract(phase + 0.5);
    float weight = 1.0 - abs(phase * 2.0 - 1.0);
    float stretch = sqrt(1.0 + min(length(velocity) * 0.35, 1.5));
    vec2 a = water_froth_flow_domain(positionM, velocity, (phase - 0.5) * 4.0);
    vec2 b = water_froth_flow_domain(positionM, velocity, (other - 0.5) * 4.0);
    float fa = water_froth_coverage_flow(a, authoredScale, footprintM * stretch, density, velocity);
    float fb = water_froth_coverage_flow(b, authoredScale, footprintM * stretch, density, velocity);
    return mix(fb, fa, weight);
}
#endif
