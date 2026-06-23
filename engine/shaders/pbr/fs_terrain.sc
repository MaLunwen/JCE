$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

#include <bgfx_shader.sh>
#include "pbr_common.sh"

// Material uniforms
uniform vec4 u_baseColorFactor;
uniform vec4 u_pbrParams;       // x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
uniform vec4 u_emissiveFactor;  // xyz=emissive, w=alphaMode (0=opaque, 1=mask, 2=blend)
uniform vec4 u_cameraPos;       // xyz=world-space camera position
uniform vec4 u_normalScale;     // x=normal map scale (x<0 => checker fallback), y=doubleSided flag
uniform vec4 u_ambientColor;    // xyz=ambient color, w=ambient intensity

// Light uniforms
// Directional lights: 2 vec4 per light, max 2 lights = 4 vec4
//   [i*2+0] = dir.xyz, intensity
//   [i*2+1] = color.xyz, unused
uniform vec4 u_dirLights[4];

// Point lights: 2 vec4 per light, max 16 lights = 32 vec4
//   [i*2+0] = pos.xyz, radius
//   [i*2+1] = color.xyz, intensity
uniform vec4 u_pointLights[32];

// Spot lights: 4 vec4 per light, max 4 lights = 16 vec4
//   [i*4+0] = pos.xyz, radius
//   [i*4+1] = dir.xyz, intensity
//   [i*4+2] = color.xyz, innerConeCos
//   [i*4+3] = outerConeCos, 0, 0, 0
uniform vec4 u_spotLights[16];

// x=numDir, y=numPoint, z=numSpot, w=shadow directional slot (index+1, 0=none)
uniform vec4 u_lightCounts;

// Texture samplers
SAMPLER2D(s_albedo,     0);
SAMPLER2D(s_metalRough, 1);
SAMPLER2D(s_normalMap,  2);
SAMPLER2D(s_aoMap,      3);
SAMPLER2D(s_emissive,   4);
SAMPLER2D(s_shadowMap,  5);

// Shadow uniforms
uniform mat4 u_shadowVP;

// Cascaded Shadow Map uniforms
// u_csmVP[i] = light VP matrix for cascade i
// u_csmSplits.xyzw = view-space split distances for cascades 0-3
uniform mat4 u_csmVP[4];
uniform vec4 u_csmSplits;
// u_csmParams.x = 1 / shadowMapSize
// u_csmParams.y = cascade blend ratio (fraction of cascade depth range)
// u_csmParams.z = normal bias strength
// u_csmParams.w = filter radius multiplier
uniform vec4 u_csmParams;
// Per-cascade bias scale (x..w for cascades 0..3)
uniform vec4 u_csmBiasScales;

// Shadow FILTER quality tier — same contract as fs_pbr.sc:
//   x < 0.5 -> 1 hard tap, x < 1.5 -> 3x3 PCF, else rotated 5x5.
// Frame-constant uniform branch set by the scene renderer from the
// render-pipeline asset's shadow_filter_quality knob.
uniform vec4 u_shadowQuality;

// IBL samplers (stages 6-8)
SAMPLERCUBE(s_irradiance, 6);
SAMPLERCUBE(s_prefilter,  7);
SAMPLER2D(s_brdfLUT,      8);

// u_iblParams.x = IBL enabled (0 or 1), y = max prefilter mip level
// u_iblParams.w = linear output flag (1=skip gamma, for tonemap pass)
uniform vec4 u_iblParams;

// CSM cascade samplers (stages 9-12)
SAMPLER2D(s_csmShadow0, 9);
SAMPLER2D(s_csmShadow1, 10);
SAMPLER2D(s_csmShadow2, 11);
SAMPLER2D(s_csmShadow3, 12);

// --- Terrain-specific bindings ----------------------------------- //
// Splat weights (stage 13): RGBA = layer0..layer3 weight.
// Layer albedo textures (stages 14, 15, plus reusing stage 4 = s_emissive
// slot for layer3, since terrain has no emissive map). Slot map:
//   stage 0  = s_albedo    -> layer0 albedo  (already declared above)
//   stage 4  = s_emissive  -> layer3 albedo  (reuse; emissive = 0 for terrain)
//   stage 13 = s_splatMap  -> RGBA splat
//   stage 14 = s_layer1
//   stage 15 = s_layer2
SAMPLER2D(s_splatMap, 13);
SAMPLER2D(s_layer1,   14);
SAMPLER2D(s_layer2,   15);

// u_terrainParams.x = layer tile scale (UV multiplier for per-layer albedo)
// u_terrainParams.y = splat enabled (1 = sample splat, 0 = layer0 only)
uniform vec4 u_terrainParams;

// Convert NDC depth to [0,1] range for shadow comparison.
// OpenGL (GLSL): NDC z is in [-1,1], needs remap.
// D3D / Vulkan / Metal: NDC z is already in [0,1].
float toShadowDepth(float ndc_z)
{
#if BGFX_SHADER_LANGUAGE_GLSL
    return ndc_z * 0.5 + 0.5;
#else
    return ndc_z;
#endif
}

float csm_sample_depth(int cascade, vec2 uv)
{
    if      (cascade == 0) return texture2D(s_csmShadow0, uv).r;
    else if (cascade == 1) return texture2D(s_csmShadow1, uv).r;
    else if (cascade == 2) return texture2D(s_csmShadow2, uv).r;
    return texture2D(s_csmShadow3, uv).r;
}

vec4 csm_clip_for_cascade(int cascade, vec3 world_pos)
{
    if      (cascade == 0) return mul(u_csmVP[0], vec4(world_pos, 1.0));
    else if (cascade == 1) return mul(u_csmVP[1], vec4(world_pos, 1.0));
    else if (cascade == 2) return mul(u_csmVP[2], vec4(world_pos, 1.0));
    return mul(u_csmVP[3], vec4(world_pos, 1.0));
}

float csm_bias_scale_for_cascade(int cascade)
{
    if      (cascade == 0) return u_csmBiasScales.x;
    else if (cascade == 1) return u_csmBiasScales.y;
    else if (cascade == 2) return u_csmBiasScales.z;
    return u_csmBiasScales.w;
}

// Stable per-fragment hash for PCF kernel rotation (breaks grid patterns).
float shadow_hash(vec3 p)
{
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float sample_csm_shadow(int cascade,
                        vec3 world_pos,
                        vec3 shading_normal,
                        vec3 to_light_dir)
{
    // World-space normal-offset bias: push the shadow sample point along
    // the surface normal to prevent light bleeding through thin geometry
    // and self-shadowing on angled surfaces.
    vec3 n = normalize(shading_normal);
    float ndotl = max(dot(n, to_light_dir), 0.0);
    float sin_theta = sqrt(max(1.0 - ndotl * ndotl, 0.0));

    float bias_scale = csm_bias_scale_for_cascade(cascade);
    float normal_offset = u_csmParams.z * bias_scale * max(sin_theta, 0.15);
    vec3 biased_pos = world_pos + n * normal_offset;

    vec4 csm_clip = csm_clip_for_cascade(cascade, biased_pos);
    vec3 csm_ndc = csm_clip.xyz / csm_clip.w;
    vec2 csm_uv = csm_ndc.xy * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
    csm_uv.y = 1.0 - csm_uv.y;
#endif
    float csm_z = toShadowDepth(csm_ndc.z);

    if (csm_uv.x < 0.0 || csm_uv.x > 1.0 ||
        csm_uv.y < 0.0 || csm_uv.y > 1.0 ||
        csm_z < 0.0 || csm_z > 1.0)
    {
        // Sentinel "not covered" — caller falls through to a wider cascade.
        return -1.0;
    }

    float inv_map_size = max(u_csmParams.x, 1.0 / 2048.0);
    vec2 texel = vec2_splat(inv_map_size);
    float cascade_lerp = clamp(float(cascade) * (1.0 / 3.0), 0.0, 1.0);

    // Depth bias: constant component + slope-scaled component to handle
    // grazing-angle shadow acne (parallel-stripe wood-grain pattern).
    float slope = sin_theta / max(ndotl, 0.1);
    float depth_bias = inv_map_size * mix(1.0, 2.0, cascade_lerp)
                     * bias_scale * (1.0 + slope * 4.0);
    depth_bias = min(depth_bias, 0.01);

    float filter_radius = max(u_csmParams.w, 0.5) * mix(1.0, 2.0, cascade_lerp);

    // Shadow filter tier (see u_shadowQuality; mirrors fs_pbr.sc). GLSL-120
    // safety rule: uniform branch selecting between CONSTANT-bound loops —
    // never a variable loop bound, never `continue`.
    // Tier 0: single hard tap (bias math above stays; hash rotation skipped).
    if (u_shadowQuality.x < 0.5)
    {
        float depth0 = csm_sample_depth(cascade, csm_uv);
        return (csm_z - depth_bias > depth0) ? 0.0 : 1.0;
    }

    // Tier 1: unrotated 3x3 PCF (9 taps).
    if (u_shadowQuality.x < 1.5)
    {
        float sum9 = 0.0;
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                vec2 offset = vec2(float(x), float(y)) * texel * filter_radius;
                float depth = csm_sample_depth(cascade, csm_uv + offset);
                sum9 += (csm_z - depth_bias > depth) ? 0.0 : 1.0;
            }
        }
        return sum9 / 9.0;
    }

    // Tier 2 (full): rotate PCF kernel per-fragment using world-position hash
    // to eliminate visible grid patterns while keeping temporally stable shadows.
    float angle = shadow_hash(world_pos) * 6.283185;
    float rot_c = cos(angle);
    float rot_s = sin(angle);

    float sum = 0.0;
    for (int y = -2; y <= 2; y++)
    {
        for (int x = -2; x <= 2; x++)
        {
            vec2 raw = vec2(float(x), float(y)) * texel * filter_radius;
            vec2 offset = vec2(raw.x * rot_c - raw.y * rot_s,
                               raw.x * rot_s + raw.y * rot_c);
            float depth = csm_sample_depth(cascade, csm_uv + offset);
            sum += (csm_z - depth_bias > depth) ? 0.0 : 1.0;
        }
    }
    return sum / 25.0;
}

vec3 safe_normalize_vec3(vec3 value, vec3 fallback)
{
    float len2 = dot(value, value);
    if (len2 > 1e-8)
        return value * inversesqrt(len2);
    return fallback;
}

/* The missing-texture checker now lives in pbr_common.sh
   (missing_texture_checker) — the local copy here was never called. */

void main()
{
    // --- Shadow calculation ---
    float shadow = 1.0;
    float shadowDirSlot = u_lightCounts.w;
    int shadowDirIndex = int(clamp(shadowDirSlot - 1.0, 0.0, 1.0));
    vec3 toLightDir = safe_normalize_vec3(-u_dirLights[shadowDirIndex * 2].xyz,
                                          vec3(0.0, 1.0, 0.0));
    vec3 baseNormal = normalize(v_normal);

    if (u_normalScale.y > 0.0 && !gl_FrontFacing)
    {
        baseNormal = -baseNormal;
    }

    bool shadowEnabled = (shadowDirSlot > 0.5) &&
        ((u_csmSplits.x > 0.0) || (u_csmParams.x > 0.0));

    if (shadowEnabled && u_csmSplits.x > 0.0)
    {
        float fragDepth = max(v_viewdepth, 0.0);

        int cascade = 3;
        if (fragDepth < u_csmSplits.x)      cascade = 0;
        else if (fragDepth < u_csmSplits.y)  cascade = 1;
        else if (fragDepth < u_csmSplits.z)  cascade = 2;

        int sel_cascade = cascade;
        shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir);

        // Cascade FALLTHROUGH (see fs_pbr_body.sh): a depth-bucketed fragment can
        // fall outside its cascade's light-space square; step to wider cascades
        // so it is never wrongly left fully lit (triangular bright wedges).
        if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir); }
        if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir); }
        if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir); }

        if (shadow >= 0.0 && cascade == sel_cascade && cascade < 3)
        {
            float split_start = 0.0;
            float split_end = u_csmSplits.x;
            if (cascade == 1) {
                split_start = u_csmSplits.x;
                split_end = u_csmSplits.y;
            } else if (cascade == 2) {
                split_start = u_csmSplits.y;
                split_end = u_csmSplits.z;
            }

            float split_span = split_end - split_start;
            if (split_span > 0.001)
            {
                float blend_fraction = clamp(u_csmParams.y, 0.0, 0.35);
                float blend_range = max(split_span * blend_fraction, 0.001);
                // Blend entirely within current cascade to prevent
                // 50%->100% discontinuity at cascade boundaries.
                float blend = smoothstep(split_end - blend_range,
                                         split_end,
                                         fragDepth);

                if (blend > 0.0001)
                {
                    float next_shadow = sample_csm_shadow(cascade + 1,
                                                          v_worldpos,
                                                          baseNormal,
                                                          toLightDir);
                    if (next_shadow >= 0.0)
                        shadow = mix(shadow, next_shadow, blend);
                }
            }
        }

        // Beyond all cascades: lit.  Then soft shadow-distance fade (u_csmSplits.w
        // = shadow far) so the shadow edge is a gradient, not a hard boundary.
        if (shadow < 0.0) shadow = 1.0;
        float shadow_far = u_csmSplits.w;
        if (shadow_far > 0.0)
        {
            float fade = smoothstep(shadow_far * 0.85, shadow_far, fragDepth);
            shadow = mix(shadow, 1.0, fade);
        }
    }
    else if (shadowEnabled)
    {
        // Legacy single shadow map fallback (PCF 3x3).
        // Normal-offset bias in world space before light-space projection.
        float leg_ndotl = max(dot(baseNormal, toLightDir), 0.0);
        float leg_sin = sqrt(max(1.0 - leg_ndotl * leg_ndotl, 0.0));
        vec3 biased_worldpos = v_worldpos + baseNormal * u_csmParams.z * max(leg_sin, 0.15);

        vec4 shadowClip = mul(u_shadowVP, vec4(biased_worldpos, 1.0));
        vec3 shadowNDC  = shadowClip.xyz / shadowClip.w;
        vec2 shadowUV   = shadowNDC.xy * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
        shadowUV.y = 1.0 - shadowUV.y;
#endif
        float shadowZ   = toShadowDepth(shadowNDC.z);
        float leg_slope = leg_sin / max(leg_ndotl, 0.1);
        float shadowBias = max(u_csmParams.x, 1.0 / 2048.0)
                         * 1.5 * (1.0 + leg_slope * 4.0);
        shadowBias = min(shadowBias, 0.01);

        if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 &&
            shadowUV.y >= 0.0 && shadowUV.y <= 1.0 &&
            shadowZ >= 0.0 && shadowZ <= 1.0)
        {
            // Tier 0 (u_shadowQuality): single hard tap on the legacy
            // single-map path too — same rule as sample_csm_shadow.
            if (u_shadowQuality.x < 0.5)
            {
                float depth0 = texture2D(s_shadowMap, shadowUV).r;
                shadow = (shadowZ - shadowBias > depth0) ? 0.0 : 1.0;
            }
            else
            {
                vec2 texelSize = vec2_splat(max(u_csmParams.x, 1.0 / 2048.0));
                float sum = 0.0;
                for (int sy = -1; sy <= 1; sy++)
                {
                    for (int sx = -1; sx <= 1; sx++)
                    {
                        float depth = texture2D(s_shadowMap, shadowUV + vec2(float(sx), float(sy)) * texelSize).r;
                        sum += (shadowZ - shadowBias > depth) ? 0.0 : 1.0;
                    }
                }
                shadow = sum / 9.0;
            }
        }
    }

    // --- Base color (terrain: 4-layer splat blend) ---
    // Per-layer UVs are tiled across the terrain so each layer's texture
    // detail is independent of the global terrain UV (which is 0..1 over
    // the full extent).
    vec2 layerUV = v_texcoord0 * max(u_terrainParams.x, 0.0001);

    vec3 a0 = texture2D(s_albedo,   layerUV).rgb;
    vec3 a1 = texture2D(s_layer1,   layerUV).rgb;
    vec3 a2 = texture2D(s_layer2,   layerUV).rgb;
    vec3 a3 = texture2D(s_emissive, layerUV).rgb; // s_emissive slot reused as layer3
    vec3 a0L = pow(clamp(a0, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2));
    vec3 a1L = pow(clamp(a1, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2));
    vec3 a2L = pow(clamp(a2, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2));
    vec3 a3L = pow(clamp(a3, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2));

    vec4 splat = vec4(1.0, 0.0, 0.0, 0.0);
    if (u_terrainParams.y > 0.5) {
        splat = texture2D(s_splatMap, v_texcoord0);
        // Re-normalize (guard against unweighted authoring or all-zero pixels).
        float wsum = splat.r + splat.g + splat.b + splat.a;
        if (wsum > 0.0001) splat /= wsum;
        else splat = vec4(1.0, 0.0, 0.0, 0.0);
    }

    vec3 albedo = (a0L * splat.r + a1L * splat.g + a2L * splat.b + a3L * splat.a)
                * u_baseColorFactor.rgb;
    float alpha = u_baseColorFactor.a;

    // --- View-mode override: TEXTURED / WIREFRAME_TEXTURED → unlit albedo ---
    // u_normalScale.z carries view mode (0=shaded, 1=wireframe(no override),
    // 2=textured/unlit, 3=wireframe+textured).
    float viewMode = u_normalScale.z;
    if (viewMode > 1.5)
    {
        // Output albedo without lighting (gamma-correct for display).
        vec3 outRgb = pow(max(albedo, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));
        gl_FragColor = vec4(outRgb, alpha);
        return;
    }

    // --- Alpha mode ---
    float alphaMode = u_emissiveFactor.w;
    float alphaCutoff = u_pbrParams.w;

    // Mask mode: discard fragments below cutoff
    if (alphaMode == 1.0 && alpha < alphaCutoff)
    {
        discard;
    }

    // --- Normal (terrain: vertex normal only, no normal map) ---
    vec3 N = baseNormal;

    // --- Metallic / Roughness (terrain: uniform-only, no MR map) ---
    float metallic  = u_pbrParams.x;
    float roughness = clamp(u_pbrParams.y, 0.04, 1.0);

    // Low-cost specular AA from normal derivatives (Toksvig-like).
    vec3 dndx = dFdx(N);
    vec3 dndy = dFdy(N);
    float normal_variance = clamp(max(dot(dndx, dndx), dot(dndy, dndy)), 0.0, 1.0);
    float aa_roughness = sqrt(normal_variance * 0.5);
    roughness = clamp(max(roughness, aa_roughness), 0.04, 1.0);

    // --- AO (terrain: uniform strength, no AO map) ---
    float ao = mix(1.0, 1.0, u_pbrParams.z);

    // --- View direction ---
    vec3 V = normalize(u_cameraPos.xyz - v_worldpos);

    // --- F0: reflectance at normal incidence ---
    vec3 F0 = vec3_splat(0.04);
    F0 = mix(F0, albedo, metallic);

    // --- Lighting accumulation ---
    vec3 Lo = vec3_splat(0.0);

    // --- Directional lights ---
    int numDirLights = int(u_lightCounts.x);
    for (int i = 0; i < 2; i++)
    {
        if (i >= numDirLights) break;

        vec3 lightDir   = normalize(-u_dirLights[i * 2 + 0].xyz);
        float intensity = u_dirLights[i * 2 + 0].w;
        vec3 lightColor = u_dirLights[i * 2 + 1].xyz;

        vec3 radiance = lightColor * intensity;
        float lightShadow = (abs(float(i) - float(shadowDirIndex)) < 0.5) ? shadow : 1.0;
        Lo += cookTorranceBRDF(N, V, lightDir, F0, albedo, metallic, roughness) * radiance * lightShadow;
    }

    // --- Point lights ---
    int numPointLights = int(u_lightCounts.y);
    for (int i = 0; i < 16; i++)
    {
        if (i >= numPointLights) break;

        vec3 lightPos   = u_pointLights[i * 2 + 0].xyz;
        float radius    = u_pointLights[i * 2 + 0].w;
        vec3 lightColor = u_pointLights[i * 2 + 1].xyz;
        float intensity = u_pointLights[i * 2 + 1].w;

        vec3 toLight  = lightPos - v_worldpos;
        float dist    = length(toLight);
        vec3 lightDir = toLight / max(dist, 0.0001);

        // Distance attenuation with radius falloff
        float attenuation = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;

        vec3 radiance = lightColor * intensity * attenuation;
        Lo += cookTorranceBRDF(N, V, lightDir, F0, albedo, metallic, roughness) * radiance;
    }

    // --- Spot lights ---
    int numSpotLights = int(u_lightCounts.z);
    for (int i = 0; i < 4; i++)
    {
        if (i >= numSpotLights) break;

        vec3 lightPos    = u_spotLights[i * 4 + 0].xyz;
        float radius     = u_spotLights[i * 4 + 0].w;
        vec3 spotDir     = normalize(u_spotLights[i * 4 + 1].xyz);
        float intensity  = u_spotLights[i * 4 + 1].w;
        vec3 lightColor  = u_spotLights[i * 4 + 2].xyz;
        float innerCos   = u_spotLights[i * 4 + 2].w;
        float outerCos   = u_spotLights[i * 4 + 3].x;

        vec3 toLight  = lightPos - v_worldpos;
        float dist    = length(toLight);
        vec3 lightDir = toLight / max(dist, 0.0001);

        // Distance attenuation with radius falloff
        float attenuation = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
        attenuation *= attenuation;

        // Cone attenuation: smoothstep between outer and inner
        float theta = dot(lightDir, -spotDir);
        float epsilon = innerCos - outerCos;
        float cone = clamp((theta - outerCos) / max(epsilon, 0.0001), 0.0, 1.0);

        vec3 radiance = lightColor * intensity * attenuation * cone;
        Lo += cookTorranceBRDF(N, V, lightDir, F0, albedo, metallic, roughness) * radiance;
    }

    // --- Ambient (IBL or flat) ---
    vec3 ambient;
    if (u_iblParams.x > 0.5)
    {
        // IBL ambient: split-sum approximation
        float NdotV_a = max(dot(N, V), 0.0);
        vec3 F_ibl = fresnelSchlickRoughness(NdotV_a, F0, roughness);
        vec3 kS_ibl = F_ibl;
        vec3 kD_ibl = (vec3_splat(1.0) - kS_ibl) * (1.0 - metallic);

        // Diffuse: sample irradiance cubemap
        vec3 irradiance = textureCube(s_irradiance, N).rgb;
        vec3 diffuseIBL = kD_ibl * irradiance * albedo;

        // Specular: sample prefiltered env + BRDF LUT
        vec3 R = reflect(-V, N);
        float maxMipLevel = u_iblParams.y;
        float perceptual_roughness = sqrt(roughness);
        vec3 prefilteredColor = textureCubeLod(s_prefilter, R,
                               perceptual_roughness * maxMipLevel).rgb;
        vec2 brdfSample = texture2D(s_brdfLUT, vec2(NdotV_a, roughness)).rg;
        vec3 specularIBL = prefilteredColor * (F_ibl * brdfSample.x + vec3_splat(brdfSample.y));

        ambient = (diffuseIBL + specularIBL) * ao;
    }
    else
    {
        ambient = u_ambientColor.xyz * u_ambientColor.w * albedo * ao;
    }

    // --- Emissive (terrain: none; slot 4 is reused as layer3 albedo) ---
    vec3 emissive = u_emissiveFactor.xyz;

    // --- Final color ---
    vec3 color = ambient + Lo + emissive;

    // --- Gamma correction (linear -> sRGB) ---
    // When postfx tonemap is enabled, keep linear output for post-processing.
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));

    // --- Output ---
    if (alphaMode == 2.0)
    {
        gl_FragColor = vec4(color, alpha);
    }
    else
    {
        gl_FragColor = vec4(color, 1.0);
    }
}
