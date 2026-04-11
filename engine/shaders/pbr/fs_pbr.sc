$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent

#include <bgfx_shader.sh>
#include "pbr_common.sh"

// Material uniforms
uniform vec4 u_baseColorFactor;
uniform vec4 u_pbrParams;       // x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
uniform vec4 u_emissiveFactor;  // xyz=emissive, w=alphaMode (0=opaque, 1=mask, 2=blend)
uniform vec4 u_cameraPos;       // xyz=world-space camera position
uniform vec4 u_normalScale;     // x=normal map scale, y=doubleSided flag
uniform vec4 u_ambientColor;    // xyz=ambient color, w=ambient intensity

// Light uniforms
// Directional lights: 2 vec4 per light, max 2 lights = 4 vec4
//   [i*2+0] = dir.xyz, intensity
//   [i*2+1] = color.xyz, unused
uniform vec4 u_dirLights[4];

// Point lights: 2 vec4 per light, max 8 lights = 16 vec4
//   [i*2+0] = pos.xyz, radius
//   [i*2+1] = color.xyz, intensity
uniform vec4 u_pointLights[16];

// x=numDir, y=numPoint, z=numSpot
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

void main()
{
    // --- Shadow calculation helper (PCF 2x2) ---
    // Transform world position to shadow clip space, then to UV.
    vec4 shadowClip = mul(u_shadowVP, vec4(v_worldpos, 1.0));
    vec3 shadowNDC  = shadowClip.xyz / shadowClip.w;
    vec2 shadowUV   = shadowNDC.xy * 0.5 + 0.5;
    // bgfx flips Y for some backends
#if BGFX_SHADER_LANGUAGE_GLSL
    float shadowZ   = shadowNDC.z * 0.5 + 0.5;
#else
    float shadowZ   = shadowNDC.z;
#endif
    float shadowBias = 0.002;
    float shadow = 1.0;
    if (shadowUV.x >= 0.0 && shadowUV.x <= 1.0 &&
        shadowUV.y >= 0.0 && shadowUV.y <= 1.0 &&
        shadowZ >= 0.0 && shadowZ <= 1.0)
    {
        vec2 texelSize = vec2_splat(1.0 / 2048.0);
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

    // --- Base color ---
    // Base color texture is sRGB-encoded (glTF spec §5.19). Convert texture
    // to linear space FIRST, then multiply by the linear baseColorFactor.
    vec4 texColor = texture2D(s_albedo, v_texcoord0);
    vec3 albedo = pow(clamp(texColor.rgb, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2))
               * u_baseColorFactor.rgb;
    float alpha = texColor.a * u_baseColorFactor.a;

    // --- Alpha mode ---
    float alphaMode = u_emissiveFactor.w;
    float alphaCutoff = u_pbrParams.w;

    // Mask mode: discard fragments below cutoff
    if (alphaMode == 1.0 && alpha < alphaCutoff)
    {
        discard;
    }

    // --- Normal ---
    vec3 N = normalize(v_normal);

    // Double-sided: flip normal if back-facing
    if (u_normalScale.y > 0.0 && !gl_FrontFacing)
    {
        N = -N;
    }

    // Perturb normal from normal map
    if (u_normalScale.x > 0.0)
    {
        vec3 tangentNormal = texture2D(s_normalMap, v_texcoord0).xyz * 2.0 - vec3_splat(1.0);
        tangentNormal.xy *= u_normalScale.x;
        tangentNormal = normalize(tangentNormal);
        mat3 TBN = mat3(normalize(v_tangent), normalize(v_bitangent), N);
        N = normalize(mul(tangentNormal, TBN));
    }

    // --- Metallic / Roughness ---
    vec4 mrSample = texture2D(s_metalRough, v_texcoord0);
    float metallic  = mrSample.b * u_pbrParams.x;
    float roughness = mrSample.g * u_pbrParams.y;
    roughness = clamp(roughness, 0.04, 1.0);

    // --- AO ---
    float ao = texture2D(s_aoMap, v_texcoord0).r;
    ao = mix(1.0, ao, u_pbrParams.z); // aoStrength blend

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
        Lo += cookTorranceBRDF(N, V, lightDir, F0, albedo, metallic, roughness) * radiance * shadow;
    }

    // --- Point lights ---
    int numPointLights = int(u_lightCounts.y);
    for (int i = 0; i < 8; i++)
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

    // --- Ambient ---
    vec3 ambient = u_ambientColor.xyz * u_ambientColor.w * albedo * ao;

    // --- Emissive ---
    // Emissive texture is also sRGB-encoded; convert to linear first,
    // then multiply by the linear emissive factor.
    vec3 emissiveTex = pow(clamp(texture2D(s_emissive, v_texcoord0).rgb, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2));
    vec3 emissive = emissiveTex * u_emissiveFactor.xyz;

    // --- Final color ---
    vec3 color = ambient + Lo + emissive;

    // --- Gamma correction (linear -> sRGB) ---
    // No tonemapping: match standard 3D viewers (Blender, glTF-viewer, etc.)
    color = pow(color, vec3_splat(1.0 / 2.2));

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
