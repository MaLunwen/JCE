#ifndef PBR_COMMON_SH
#define PBR_COMMON_SH

#define PI 3.14159265359

// Fresnel-Schlick approximation
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (vec3_splat(1.0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// GGX/Trowbridge-Reitz normal distribution function
float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0001);
}

// Schlick-GGX geometry function (single direction)
float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    float denom = NdotV * (1.0 - k) + k;

    return NdotV / max(denom, 0.0001);
}

// Smith's method combining geometry obstruction and shadowing
float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1  = geometrySchlickGGX(NdotV, roughness);
    float ggx2  = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Full Cook-Torrance BRDF: specular + Lambertian diffuse
vec3 cookTorranceBRDF(vec3 N, vec3 V, vec3 L, vec3 F0, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // Specular terms
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator   = D * G * F;
    float denominator = 4.0 * NdotV * NdotL;
    vec3 specular    = numerator / max(denominator, 0.001);

    // Energy conservation: diffuse portion
    vec3 kS = F;
    vec3 kD = vec3_splat(1.0) - kS;
    kD *= (1.0 - metallic); // Metals have no diffuse

    // Lambertian diffuse
    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * NdotL;
}

// Fresnel-Schlick with roughness for IBL ambient specular
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    vec3 oneMinusRough = vec3_splat(1.0 - roughness);
    // max(oneMinusRough, F0) per-component
    vec3 maxVal = max(oneMinusRough, F0);
    return F0 + (maxVal - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// ---------------------------------------------------------------------
// Variance Shadow Maps (VSM) sampling
// ---------------------------------------------------------------------
// Read the (depth, depth^2) moments from a VSM target and return the
// lit-fraction upper bound at test depth `t`.
// `min_variance` clamps numerical noise (~1e-5 typical).
// CPU reference: jce_shadow_filter_vsm_chebyshev() in
//                engine/include/jce/renderer/jce_shadow_filter.h
float sample_vsm_chebyshev(vec2 moments, float t, float min_variance)
{
    if (t <= moments.x) return 1.0;

    float variance = moments.y - moments.x * moments.x;
    variance = max(variance, min_variance);

    float d   = t - moments.x;
    float p_max = variance / (variance + d * d);
    return p_max;
}

// Light-bleed reduction: rescales `p` so values below `amount` clamp
// to 0 and the rest linearly remap to [0,1].  Mitigates the classic
// VSM over-soft halo around occluders.  Typical `amount` 0.1 - 0.3.
// CPU reference: jce_shadow_filter_vsm_reduce_bleed().
float sample_vsm_reduce_bleed(float p, float amount)
{
    return clamp((p - amount) / max(1.0 - amount, 1e-5), 0.0, 1.0);
}

// Convenience: full VSM lookup with default constants matching the
// CPU helpers' typical call sites.
float sample_vsm(sampler2D vsm_target, vec2 uv, float t,
                 float min_variance, float bleed_reduction)
{
    vec4 sm = texture2D(vsm_target, uv);
    float p = sample_vsm_chebyshev(sm.xy, t, min_variance);
    return sample_vsm_reduce_bleed(p, bleed_reduction);
}

#endif // PBR_COMMON_SH
