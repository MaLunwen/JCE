$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

#include <bgfx_shader.sh>
#include "pbr_common.sh"
#include "fog_apply.sh"
#include "shadow_debug.sh"

// Material uniforms
uniform vec4 u_baseColorFactor;
uniform vec4 u_pbrParams;       // x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
uniform vec4 u_ssaoParams;      // x=SSAO active, yz=1/AO target size, w=contact shadows
uniform vec4 u_weatherSurface;  // x=wetness y=snow zw=reserved
uniform vec4 u_terrainSnowLook; // brightness, warmth, low relief tint, micro-normal strength
#include "wetness.sh"
#include "area_light.sh"
uniform vec4 u_emissiveFactor;  // xyz=emissive, w=alphaMode (0=opaque, 1=mask, 2=blend)
uniform vec4 u_cameraPos;       // xyz=world-space camera position
uniform vec4 u_normalScale;     // x=normal map scale (x<0 => checker fallback), y=doubleSided flag
uniform vec4 u_ambientColor;    // xyz=ambient color, w=ambient intensity

// ── Stylized Look Profile (consumed; uploaded by renderer, plan-02).
//   u_lookWrap = {wrap_factor, rim_power, rim_intensity, toonFlag}
//   u_lookRim  = {rim_color.rgb, pad}
//   u_lookHemiGround = {ambient_ground_color.rgb, hemi_enabled}
uniform vec4 u_lookWrap;
uniform vec4 u_lookRim;
uniform vec4 u_lookHemiGround;

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
/* Stage 1 was s_metalRough: DECLARED and never read on the terrain path --
 * terrain takes metallic and roughness from u_pbrParams, not from a texture.
 * It now carries the screen-space ambient occlusion target, which terrain had
 * no way to receive at all: every one of terrain's sixteen stages was spoken
 * for on paper, and two of them (this and stage 2) were spoken for by
 * samplers nothing sampled.
 *
 * Screen UV, exactly as fs_pbr_body.sh does it, so the ground and the things
 * standing on it darken by the same rule at the same contact. */
SAMPLER2D(s_terrainAO, 1);
// Four tangent-space normals followed by four packed material masks:
// mask R=metallic, G=AO, B=height, A=smoothness.  A 2D array keeps all eight
// inputs in the one sampler stage terrain can spare.
SAMPLER2DARRAY(s_terrainLayerData, 2);
uniform vec4 u_terrainNormalScales;
uniform vec4 u_terrainMaskFlags;
/* Stage 3 was s_aoMap: DECLARED but never bound and never read on the terrain
 * path (terrain has its own draw path and binds nothing here).  An unbound
 * sampler is harmless until something reads it and undefined the moment
 * anything does -- which is exactly how a previous cloud-shadow attempt turned
 * the whole ground black.  Reusing the stage rather than leaving the hazard in
 * place is what makes room for the cloud shadow at zero cost: terrain really
 * does occupy all 16 stages (0-8 material/IBL, 9-12 CSM cascades, 13 splat,
 * 14-15 layers), so this was the only one available. */
SAMPLER2D(s_cloudShadow, 3);
/* x = world extent the map covers (0 = no cloud shadow)
 * yz = map centre in world XZ
 * w  = strength (0..1) */
uniform vec4 u_cloudShadow;
SAMPLER2D(s_emissive,   4);
/* s_shadowMap (stage 5) is declared by csm_shadow.sh, included below: under
 * CSM that stage carries the DYNAMIC-caster atlas and the header is what
 * samples it, so the declaration belongs with the code that owns the meaning.
 * The single-light path further down still uses the same name for the same
 * stage -- the two are mutually exclusive at the bind site. */

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
// Per-cascade world-units-per-shadow-texel scale, used to turn a normalized
// shadow-depth difference into a penumbra width.  Written by jce_sr_shadow.c
// from JceCsmData.depth_range; see pcss.sh.
uniform vec4 u_csmPenumbra;

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
// u_terrainParams.y = 0/1 base/splat, -1/-2 base/splat with optional snow look
// u_terrainParams.z = stamp count, or -(16 * cover count + stamp count)
// u_terrainParams.w = height-aware blend strength (0 = legacy splat blend)
#include "terrain_snow_height.sh"

// Per-tile splat UV remap (large-world #4): for a streamed/tiled terrain the
// splat map is a PER-TILE texture, so the global terrain UV (v_texcoord0, 0..1
// over the whole extent) must be mapped into the bound tile's local [0..1].
//   splatUV = (v_texcoord0 - u_terrainTileUV.xy) * u_terrainTileUV.zw
// xy = tile UV origin, zw = tile UV scale.  Monolithic terrains set {0,0,1,1}
// so the remap is identity (byte-identical sampling).
uniform vec4 u_terrainTileUV;

/* Stable world-space basin potential. A second frequency breaks up the
 * low-frequency shapes without allocating another terrain sampler. */
float terrain_puddle_hash(vec2 cell)
{
    vec3 p = fract(vec3(cell.x, cell.y, cell.x) * 0.1031);
    p += vec3_splat(dot(p, p.yzx + vec3_splat(33.33)));
    return fract((p.x + p.y) * p.z);
}

float terrain_puddle_noise(vec2 world_xz)
{
    vec2 cell = floor(world_xz);
    vec2 f = fract(world_xz);
    f = f * f * (vec2_splat(3.0) - 2.0 * f);
    float lower = mix(terrain_puddle_hash(cell),
                      terrain_puddle_hash(cell + vec2(1.0, 0.0)), f.x);
    float upper = mix(terrain_puddle_hash(cell + vec2(0.0, 1.0)),
                      terrain_puddle_hash(cell + vec2(1.0, 1.0)), f.x);
    return mix(lower, upper, f.y);
}

// Analytic XZ gradient of a cubic value-noise height sample. The four
// corners are shared by both partials; the derivative vanishes at cell seams.
vec2 terrain_snow_grain_gradient(vec2 p)
{
    vec2 cell = floor(p);
    vec2 t = fract(p);
    vec2 s = t * t * (vec2_splat(3.0) - 2.0 * t);
    vec2 ds = 6.0 * t * (vec2_splat(1.0) - t);
    float a = terrain_puddle_hash(cell);
    float b = terrain_puddle_hash(cell + vec2(1.0, 0.0));
    float c = terrain_puddle_hash(cell + vec2(0.0, 1.0));
    float d = terrain_puddle_hash(cell + vec2(1.0, 1.0));
    return ds * vec2(mix(b - a, d - c, s.y), mix(c - a, d - b, s.x));
}

float terrain_puddle_mask(vec2 world_xz, float slope_up, float wet,
                          float exposure, float snow)
{
    float flatness = smoothstep(0.94, 0.995, slope_up);
    float water = wet * flatness * mix(0.72, 1.0, exposure);
    if (exposure <= 0.0 || water <= 0.18) return 0.0;
    float basin = mix(terrain_puddle_noise(world_xz * 0.44),
                      terrain_puddle_noise(world_xz * 1.40 + vec2(19.3, -7.1)), 0.25);
    float threshold = mix(0.18, 1.02, basin);
    return smoothstep(threshold, threshold + 0.12, water) * exposure
         * (1.0 - smoothstep(0.12, 0.60, snow));
}

// Convert NDC depth to [0,1] range for shadow comparison.
// OpenGL (GLSL): NDC z is in [-1,1], needs remap.
// D3D / Vulkan / Metal: NDC z is already in [0,1].
#include "csm_shadow.sh"

#include "cloud_shadow.sh"

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

    /* Carries the cascade that actually shadowed this fragment, for view
     * mode 9.  Terrain takes it from csm_shadow_factor_dbg rather than
     * recomputing, for the same reason the mesh path does: a visualiser that
     * derives its own answer can agree with itself while disagreeing with the
     * renderer. */
    float dbgCascade = -1.0;

    bool shadowEnabled = (shadowDirSlot > 0.5) &&
        ((u_csmSplits.x > 0.0) || (u_csmParams.x > 0.0));

    if (shadowEnabled && u_csmSplits.x > 0.0)
    {
        shadow = csm_shadow_factor_dbg(v_worldpos, baseNormal, toLightDir,
                                       v_viewdepth, dbgCascade);

        /* Cloud shadow attenuates the DIRECT sun, which is the whole point of
         * having one: a sky with clouds in it and ground lighting that never
         * changes reads as a painted backdrop.  It multiplies the CSM factor
         * rather than the ambient, because a cloud blocks the sun, not the sky
         * -- attenuating ambient instead would darken the shadowed side of
         * everything, which is the opposite of what a passing cloud does.
         *
         * Guarded on extent: the C side leaves it at 0 whenever the bake is
         * stale or absent, so the branch fails toward full sunlight rather
         * than toward an unbound texture read. */
        shadow *= cloud_shadow_at(v_worldpos.xz);


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
    // The four layer albedos are sRGB-encoded and the SAMPLER decodes them:
    // each layer texture carries a hardware sRGB view, so the decode happens
    // per texel BEFORE filtering rather than per fragment after it.  See
    // fs_pbr_body.sh for why that distinction is the whole point.
    vec3 a0L = clamp(a0, vec3_splat(0.0), vec3_splat(1.0));
    vec3 a1L = clamp(a1, vec3_splat(0.0), vec3_splat(1.0));
    vec3 a2L = clamp(a2, vec3_splat(0.0), vec3_splat(1.0));
    vec3 a3L = clamp(a3, vec3_splat(0.0), vec3_splat(1.0));

    vec4 splat = vec4(1.0, 0.0, 0.0, 0.0);
    bool useSnowLook = u_terrainParams.y == -1.0 || u_terrainParams.y == -2.0;
    if (u_terrainParams.y > 0.5 || u_terrainParams.y == -2.0) {
        vec2 splatUV = (v_texcoord0 - u_terrainTileUV.xy) * u_terrainTileUV.zw;
        splat = texture2D(s_splatMap, splatUV);
        // Re-normalize (guard against unweighted authoring or all-zero pixels).
        float wsum = splat.r + splat.g + splat.b + splat.a;
        if (wsum > 0.0001) splat /= wsum;
        else splat = vec4(1.0, 0.0, 0.0, 0.0);
    }

    vec4 maskFlags = clamp(u_terrainMaskFlags, vec4_splat(0.0),
                           vec4_splat(1.0));
    vec4 m0 = vec4(0.0, 1.0, 0.5, 0.0);
    vec4 m1 = m0;
    vec4 m2 = m0;
    vec4 m3 = m0;
    if (maskFlags.x > 0.5)
        m0 = texture2DArray(s_terrainLayerData, vec3(layerUV, 4.0));
    if (maskFlags.y > 0.5)
        m1 = texture2DArray(s_terrainLayerData, vec3(layerUV, 5.0));
    if (maskFlags.z > 0.5)
        m2 = texture2DArray(s_terrainLayerData, vec3(layerUV, 6.0));
    if (maskFlags.w > 0.5)
        m3 = texture2DArray(s_terrainLayerData, vec3(layerUV, 7.0));

    float heightBlend = clamp(u_terrainParams.w, 0.0, 1.0);
    if (heightBlend > 0.0001 &&
        dot(maskFlags, vec4_splat(1.0)) > 0.5) {
        vec4 heights = mix(vec4_splat(0.5),
                           vec4(m0.b, m1.b, m2.b, m3.b), maskFlags);
        vec4 weightedHeight = heights * heightBlend + splat;
        float peak = max(max(weightedHeight.x, weightedHeight.y),
                         max(weightedHeight.z, weightedHeight.w));
        float transition = max(0.02, (1.0 - heightBlend) * 0.5);
        vec4 heightWeights = max(
            weightedHeight - vec4_splat(peak - transition),
            vec4_splat(0.0)) * splat;
        float heightSum = dot(heightWeights, vec4_splat(1.0));
        if (heightSum > 0.0001)
            splat = heightWeights / heightSum;
    }

    vec3 albedo = (a0L * splat.r + a1L * splat.g + a2L * splat.b + a3L * splat.a)
                * u_baseColorFactor.rgb;
    float alpha = u_baseColorFactor.a;

    // --- View-mode override: TEXTURED / WIREFRAME_TEXTURED → unlit albedo ---
    // u_normalScale.z carries view mode (0=shaded, 1=wireframe(no override),
    // 2=textured/unlit, 3=wireframe+textured).
    float viewMode = u_normalScale.z;

    /* Depth and shadow views report the results of this terrain draw. */
    if (viewMode > 7.5)
    {
        vec3 dbg;
        if      (viewMode < 8.5) dbg = shadow_debug_depth_color(v_viewdepth, u_csmSplits.w);
        else if (viewMode < 9.5) dbg = shadow_debug_cascade_color(dbgCascade);
        else                     dbg = vec3_splat(shadow);
        gl_FragColor = vec4(dbg, 1.0);
        return;
    }

    if (viewMode > 1.5 && viewMode < 3.5)
    {
        // Output albedo without lighting (gamma-correct for display).
        vec3 outRgb = pow(max(albedo, vec3_splat(0.0)), vec3_splat(u_iblParams.z));
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

    // --- Normal (four tangent-space layer normals) ---
    vec3 N = baseNormal;

    vec4 normalScales = max(u_terrainNormalScales, vec4_splat(0.0));
    bool hasLayerNormal = normalScales.x > 0.0001 ||
                          normalScales.y > 0.0001 ||
                          normalScales.z > 0.0001 ||
                          normalScales.w > 0.0001;
    if (hasLayerNormal) {
        vec3 n0 = vec3(0.0, 0.0, 1.0);
        vec3 n1 = n0;
        vec3 n2 = n0;
        vec3 n3 = n0;
        if (normalScales.x > 0.0001) {
            n0 = texture2DArray(s_terrainLayerData, vec3(layerUV, 0.0)).xyz
               * 2.0 - vec3_splat(1.0);
            n0.xy *= normalScales.x;
            n0 = normalize(n0);
        }
        if (normalScales.y > 0.0001) {
            n1 = texture2DArray(s_terrainLayerData, vec3(layerUV, 1.0)).xyz
               * 2.0 - vec3_splat(1.0);
            n1.xy *= normalScales.y;
            n1 = normalize(n1);
        }
        if (normalScales.z > 0.0001) {
            n2 = texture2DArray(s_terrainLayerData, vec3(layerUV, 2.0)).xyz
               * 2.0 - vec3_splat(1.0);
            n2.xy *= normalScales.z;
            n2 = normalize(n2);
        }
        if (normalScales.w > 0.0001) {
            n3 = texture2DArray(s_terrainLayerData, vec3(layerUV, 3.0)).xyz
               * 2.0 - vec3_splat(1.0);
            n3.xy *= normalScales.w;
            n3 = normalize(n3);
        }
        vec3 tangentNormal = normalize(n0 * splat.r + n1 * splat.g +
                                       n2 * splat.b + n3 * splat.a);
        if (dot(v_tangent, v_tangent) > 1e-8 &&
            dot(v_bitangent, v_bitangent) > 1e-8) {
            mat3 TBN = mtxFromCols(normalize(v_tangent),
                                   normalize(v_bitangent), N);
            N = normalize(mul(TBN, tangentNormal));
        }
    }

    // --- Metallic / Roughness / material AO from packed layer masks ---
    float baseMetallic = clamp(u_pbrParams.x, 0.0, 1.0);
    float baseRoughness = clamp(u_pbrParams.y, 0.04, 1.0);
    vec4 metallicLayers = vec4(
        mix(baseMetallic, m0.r, maskFlags.x),
        mix(baseMetallic, m1.r, maskFlags.y),
        mix(baseMetallic, m2.r, maskFlags.z),
        mix(baseMetallic, m3.r, maskFlags.w));
    vec4 roughnessLayers = vec4(
        mix(baseRoughness, 1.0 - m0.a, maskFlags.x),
        mix(baseRoughness, 1.0 - m1.a, maskFlags.y),
        mix(baseRoughness, 1.0 - m2.a, maskFlags.z),
        mix(baseRoughness, 1.0 - m3.a, maskFlags.w));
    vec4 aoLayers = vec4(
        mix(1.0, m0.g, maskFlags.x),
        mix(1.0, m1.g, maskFlags.y),
        mix(1.0, m2.g, maskFlags.z),
        mix(1.0, m3.g, maskFlags.w));
    float metallic = clamp(dot(metallicLayers, splat), 0.0, 1.0);
    float roughness = clamp(dot(roughnessLayers, splat), 0.04, 1.0);
    float materialAo = clamp(dot(aoLayers, splat), 0.0, 1.0);

    float wet = u_weatherSurface.x * wetness_exposure(N);
    float snow = snow_coverage(N, u_weatherSurface.y);
    float stamp_count, cover_count;
    terrain_snow_counts(stamp_count, cover_count);
    float precipitation_exposure = 1.0;
    if ((snow > 0.0 || wet > 0.0) && cover_count > 0.0) {
        vec3 base_world = vec3(v_worldpos.x, v_localpos.y, v_worldpos.z);
        precipitation_exposure = terrain_snow_exposure(base_world, cover_count);
        snow *= precipitation_exposure;
    }

    /* Shelter attenuates the ground's wet response as well as its puddles.
     * Mix lying snow afterward so earlier rain does not darken its surface. */
    float exposed_wet = wet * precipitation_exposure;
    roughness = clamp(wetness_roughness(roughness, exposed_wet), 0.04, 1.0);
    albedo    = wetness_albedo(albedo, exposed_wet);
    if (useSnowLook && snow > 0.0 &&
        (u_terrainSnowLook.x != 0.0 || u_terrainSnowLook.y != 0.0 || u_terrainSnowLook.z > 0.0)) {
        vec3 snowWhite = vec3_splat(0.90 + u_terrainSnowLook.x) +
            u_terrainSnowLook.y * vec3(0.055, 0.012, -0.065);
        if (u_terrainSnowLook.z > 0.0) {
            // Virtual snow relief, with actual R5 snow geometry reinforcing depressions.
            float relief = terrain_puddle_noise(v_worldpos.xz * 0.62);
            relief += clamp(v_worldpos.y - v_localpos.y, -u_terrainSnowGeometry.z, u_terrainSnowGeometry.w) * 1.8;
            float basin = (1.0 - smoothstep(0.20, 0.58, relief)) * u_terrainSnowLook.z;
            snowWhite *= mix(vec3_splat(1.0), vec3(0.66, 0.79, 0.90), basin);
        }
        albedo = mix(albedo, clamp(snowWhite, vec3_splat(0.0), vec3_splat(0.98)), snow);
    } else {
        albedo = snow_albedo(albedo, snow);
    }
    roughness  = clamp(snow_roughness(roughness, snow), 0.04, 1.0);
    // Accumulated snow covers the underlying sand normal. The shared signed
    // snow field and the independently controlled snow grain remain afterward.
    if (snow > 0.0)
        N = normalize(mix(N, baseNormal, 0.85 * snow));
    if (snow > 0.0 && stamp_count > 0.0) {
        // Resolved geometry uses the same quantized coverage as both vertex
        // paths. Smaller stamps retain the existing material-detail path.
        float track_snow = snow;
        if (u_terrainSnowGeometry.x > 0.0)
            track_snow = u_terrainSnowGeometry.x * precipitation_exposure *
                smoothstep(0.35, 0.85, clamp(baseNormal.y, 0.0, 1.0));
        vec3 track = terrain_snow_limit(
            terrain_snow_track(v_worldpos.xz, stamp_count, 0.0), track_snow);
        float compact = clamp(-track.x * 8.0, 0.0, 1.0);
        if (compact > 0.0) {
            albedo *= mix(vec3_splat(1.0), vec3(0.73, 0.82, 0.92), compact);
            roughness *= 1.0 - 0.22 * compact;
            // Only the already-existing signed depression reduces ambient light.
            materialAo *= 1.0 - 0.18 * compact;
        }
        vec2 gradient = track.yz;
        float slope = length(gradient);
        if (slope > 0.0) {
            gradient *= min(1.0, 0.85 / slope);
            N = normalize(N - vec3(gradient.x, 0.0, gradient.y) * max(N.y, 0.0));
        }
    }

    if (useSnowLook && u_terrainSnowLook.w > 0.0) {
        // Use quad derivatives in uniform control flow and fade subpixel grain.
        vec2 grain = v_worldpos.xz * 4.0;
        vec2 footprint = fwidth(grain);
        float grainWeight = 1.0 - smoothstep(0.28, 0.85, max(footprint.x, footprint.y));
        if (snow > 0.0 && grainWeight > 0.0) {
            vec2 slope = terrain_snow_grain_gradient(grain) *
                (0.12 * u_terrainSnowLook.w * snow * grainWeight);
            N = normalize(N - vec3(slope.x, 0.0, slope.y) * max(N.y, 0.0));
        }
    }

    float puddle = 0.0;
    if (wet > 0.0) {
        puddle = terrain_puddle_mask(v_worldpos.xz, baseNormal.y, wet,
                                    precipitation_exposure, snow);
        if (puddle > 0.0) {
            albedo *= mix(vec3_splat(1.0), vec3(0.71, 0.82, 0.88), puddle);
            roughness = mix(roughness, 0.075, puddle);
            metallic *= 1.0 - puddle;
            float flatten = 0.72 * puddle;
            N = normalize(vec3(N.x * (1.0 - flatten),
                               mix(max(N.y, 0.0), 1.0, flatten),
                               N.z * (1.0 - flatten)));
        }
    }

    // Low-cost specular AA from normal derivatives (Toksvig-like).
    vec3 dndx = dFdx(N);
    vec3 dndy = dFdy(N);
    float normal_variance = clamp(max(dot(dndx, dndx), dot(dndy, dndy)), 0.0, 1.0);
    float aa_roughness = sqrt(normal_variance * 0.5);
    roughness = clamp(max(roughness, aa_roughness), 0.04, 1.0);

    // --- AO (terrain: uniform strength, no AO map) ---
    /* Ambient occlusion.
     *
     * This read `mix(1.0, 1.0, u_pbrParams.z)` -- a constant 1.0 wearing the
     * shape of a blend, for every value of the aoStrength it appears to
     * honour. Terrain had NO occlusion of any kind: not a texture, not SSAO,
     * nothing, in a shader whose whole subject is ground that other things
     * stand on. Measured before this: turning SSAO on and off changed 59
     * pixels inside the viewport, out of 1,196,616.
     *
     * aoStrength still blends, because an authored 0 must still mean "no AO
     * on this terrain" -- the difference is that now there is something to
     * turn down. */
    float aoStrength = clamp(u_pbrParams.z, 0.0, 1.0);
    float ao = mix(1.0, materialAo, aoStrength);
    if (u_ssaoParams.x > 0.5)
    {
        float ssao = texture2D(s_terrainAO, gl_FragCoord.xy * u_ssaoParams.yz).r;
        ao *= mix(1.0, clamp(ssao, 0.0, 1.0), aoStrength);
    }

    if (viewMode > 3.5 && viewMode < 7.5)
    {
        vec3 dbg;
        if      (viewMode < 4.5) dbg = N * 0.5 + vec3_splat(0.5);
        else if (viewMode < 5.5) dbg = vec3_splat(roughness);
        else if (viewMode < 6.5) dbg = vec3_splat(metallic);
        else                     dbg = vec3_splat(ao);
        gl_FragColor = vec4(dbg, 1.0);
        return;
    }

    // --- View direction ---
    vec3 V = normalize(u_cameraPos.xyz - v_worldpos);

    // --- F0: reflectance at normal incidence ---
    vec3 F0 = vec3_splat(0.04);
    F0 = mix(F0, albedo, metallic);
    if (puddle > 0.0)
        F0 = mix(F0, vec3_splat(0.020), puddle);

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
        Lo += cookTorranceBRDFWrap(N, V, lightDir, F0, albedo, metallic, roughness, u_lookWrap.x) * radiance * lightShadow;
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
        Lo += cookTorranceBRDFWrap(N, V, lightDir, F0, albedo, metallic, roughness, u_lookWrap.x) * radiance;
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
        Lo += cookTorranceBRDFWrap(N, V, lightDir, F0, albedo, metallic, roughness, u_lookWrap.x) * radiance;
    }

    // --- Area (rect) lights ---
    // The SAME jce_area_light_contrib fs_pbr_body.sh calls, from the one
    // include: the ground under a rect light has to be lit by it, and two
    // copies of the maths would be two places for it to drift.
    {
        int numAreaLights = int(u_areaParams.x);
        for (int ai = 0; ai < JCE_MAX_AREA_LIGHTS; ai++)
        {
            if (ai >= numAreaLights) break;
            Lo += jce_area_light_contrib(ai, v_worldpos, N, V, F0, albedo,
                                         metallic, roughness, u_lookWrap.x);
        }
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
        // Two-color hemisphere (sky=u_ambientColor top, ground=u_lookHemiGround
        // bottom) by world N.y. hemi_enabled==0 => collapses to flat ambient.
        vec3 skyAmbient = u_ambientColor.xyz * u_ambientColor.w;
        float h = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 hemiColor = mix(u_lookHemiGround.xyz, skyAmbient, h);
        vec3 ambientColor = mix(skyAmbient, hemiColor, u_lookHemiGround.w);
        ambient = ambientColor * albedo * ao;
    }

    // --- World-space rim / fresnel (stylized silhouette separation) ---
    {
        float NdotV_r = clamp(dot(N, V), 0.0, 1.0);
        float rim = pow(1.0 - NdotV_r, u_lookWrap.y);
        rim *= smoothstep(0.0, 0.25, max(dot(N, toLightDir), 0.0));
        ambient += u_lookRim.xyz * (u_lookWrap.z * rim);
    }

    // --- Emissive (terrain: none; slot 4 is reused as layer3 albedo) ---
    vec3 emissive = u_emissiveFactor.xyz;

    // --- Final color ---
    vec3 color = ambient + Lo + emissive;

    // --- Aerial-perspective fog (linear, before gamma) ---
    color = apply_aerial_fog(color, v_viewdepth, v_worldpos,
                             u_cameraPos.xyz, toLightDir);

    // --- Gamma correction (linear -> sRGB) ---
    // When postfx tonemap is enabled, keep linear output for post-processing.
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(u_iblParams.z));

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
