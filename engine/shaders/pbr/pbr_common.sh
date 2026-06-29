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

// Cook-Torrance BRDF with WRAP / half-Lambert diffuse softening.
// `wrap` in [0,1]: 0 = hard Lambert (algebraically identical to
// cookTorranceBRDF since clamp((d+0)/(1+0),0,1) == max(d,0) for d in
// [-1,1]); ~0.35 softens the terminator; 1.0 = full half-Lambert.
// Only the FINAL energy term uses the wrapped NdotL; the specular
// denominator keeps the un-wrapped max(dot(N,L),0) so this collapses
// exactly to cookTorranceBRDF at wrap==0 (no specular divergence).
vec3 cookTorranceBRDFWrap(vec3 N, vec3 V, vec3 L, vec3 F0, vec3 albedo, float metallic, float roughness, float wrap)
{
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);          // un-wrapped (specular denom)
    float NdotV = max(dot(N, V), 0.0);
    float NdotL_w = clamp((dot(N, L) + wrap) / (1.0 + wrap), 0.0, 1.0); // wrapped energy term

    // Specular terms (UNCHANGED — uses un-wrapped NdotL in the denom).
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator    = D * G * F;
    float denominator = 4.0 * NdotV * NdotL;
    vec3 specular     = numerator / max(denominator, 0.001);

    // Energy conservation: diffuse portion.
    vec3 kS = F;
    vec3 kD = vec3_splat(1.0) - kS;
    kD *= (1.0 - metallic);

    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * NdotL_w;       // <- wrapped final multiply
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

// ── Missing-texture checker (fragment-only: uses derivatives) ───────

float checker_cell(vec2 uv, float scale)
{
    vec2 cell = floor(uv * scale);
    return fract((cell.x + cell.y) * 0.5) * 2.0;
}

// Magenta/black checker with derivative-based minification fade: once a
// cell shrinks below about a pixel the binary pattern aliases into
// shimmer (a real checker TEXTURE relies on mipmaps for exactly this),
// so fade to the pattern mean instead.
vec3 checker_color(vec2 coord, float cells_per_unit)
{
    const vec3 magenta = vec3(1.0, 0.0, 1.0);
    const vec3 black   = vec3(0.0, 0.0, 0.0);
    const vec3 mean    = vec3(0.5, 0.0, 0.5);

    vec2  duv = (abs(dFdx(coord)) + abs(dFdy(coord))) * cells_per_unit;
    float px  = max(duv.x, duv.y);   /* checker cells crossed per pixel */
    vec3  c   = mix(magenta, black, checker_cell(coord, cells_per_unit));
    return mix(c, mean, smoothstep(0.5, 1.0, px));
}

// Missing-texture fallback — industry-standard look (Source/UE style):
// the checker samples the mesh's OWN UV space, i.e. exactly where the
// absent texture would have mapped, so it doubles as a UV-mapping debug
// view and has NO projection seams by construction. Meshes without a
// usable UV stream (constant UVs -> zero derivatives) fall back to a
// LOCAL-space planar checker picked by the DOMINANT axis of the face
// normal. The pick is a HARD select on purpose: the previous
// implementation soft-blended three independent binary checkers by
// pow(|n|,4) weights, and any surface not facing a major axis (spheres,
// cylinders, rotated cubes) showed 30-70% mixes of disagreeing patterns
// — muddy interference bands at every projection boundary. One pattern
// per pixel keeps every cell crisp; axis transitions become thin clean
// lines.
vec3 missing_texture_checker(vec3 local_pos, vec2 uv)
{
    const float uv_cells    = 8.0;   /* cells per UV tile               */
    const float local_cells = 3.0;   /* cells per local-space unit      */

    vec2 duv = abs(dFdx(uv)) + abs(dFdy(uv));
    if (duv.x + duv.y > 1e-7)
        return checker_color(uv, uv_cells);

    vec3 an = abs(cross(dFdx(local_pos), dFdy(local_pos)));
    vec2 plane = (an.x >= an.y && an.x >= an.z) ? local_pos.yz
               : (an.y >= an.z)                 ? local_pos.xz
                                                : local_pos.xy;
    return checker_color(plane, local_cells);
}

#endif // PBR_COMMON_SH
