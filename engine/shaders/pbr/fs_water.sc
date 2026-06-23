$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

#include <bgfx_shader.sh>
#include "pbr_common.sh"

/*
 * fs_water.sc -- translucent Gerstner water surface shading (slice 2).
 *
 * This slice keeps to: Fresnel mix between color_shallow / color_deep driven
 * by view angle, alpha-blended transparency, and a single sun specular
 * highlight.  Depth-based shoreline fade and planar reflection are explicit
 * FOLLOW-UPS and are intentionally NOT implemented here.
 *
 * Lighting reuses the shared PBR directional-light + camera uniforms the
 * scene renderer already binds for every draw (sr_inline_bind_pbr_global),
 * plus the IBL params for an optional environment tint of the reflection.
 */

// Shared PBR uniforms (bound by the scene renderer's PBR global bind).
uniform vec4 u_cameraPos;       // xyz=world camera position
uniform vec4 u_dirLights[4];    // [i*2+0]=dir.xyz,intensity  [i*2+1]=color.xyz
uniform vec4 u_lightCounts;     // x=numDir
uniform vec4 u_ambientColor;    // xyz=ambient color, w=intensity
uniform vec4 u_iblParams;       // x=IBL enabled, y=maxPrefilterMip, w=linear out

// Water-specific shading uniforms (bound by sr_draw_water).
uniform vec4 u_water_color_shallow; // rgb = grazing/shallow color
uniform vec4 u_water_color_deep;    // rgb = steep/deep color
uniform vec4 u_water_shading;       // x=transparency[0..1], y=sun_specular

// IBL prefilter cube (stage 7) for an environment-tinted reflection when a
// skybox is present; falls back to a flat sky tint otherwise.
SAMPLERCUBE(s_prefilter, 7);

void main()
{
    vec3 N = normalize(v_normal);
    // Water surface always faces up toward the viewer above it; flip a
    // back-facing normal so under-the-surface / steep views stay lit.
    vec3 V = normalize(u_cameraPos.xyz - v_worldpos);
    if (dot(N, V) < 0.0) N = -N;

    float NdotV = clamp(dot(N, V), 0.0, 1.0);

    // --- Fresnel (Schlick) ---------------------------------------------
    // Looking straight down (high NdotV) -> shallow color shows through;
    // grazing angle (low NdotV) -> deep/reflective color dominates.
    float F0 = 0.02; // water at normal incidence ~2%
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    vec3 shallow = u_water_color_shallow.rgb;
    vec3 deep    = u_water_color_deep.rgb;
    vec3 baseColor = mix(shallow, deep, fresnel);

    // --- Reflection tint (environment or flat sky) ---------------------
    vec3 reflColor = vec3(0.55, 0.7, 0.9); // default soft-sky reflection
    if (u_iblParams.x > 0.5)
    {
        vec3 R = reflect(-V, N);
        float maxMip = u_iblParams.y;
        reflColor = textureCubeLod(s_prefilter, R, 0.15 * maxMip).rgb;
    }
    // Blend a touch of reflection in at grazing angles.
    vec3 color = mix(baseColor, reflColor, fresnel * 0.6);

    // --- Ambient term --------------------------------------------------
    color += u_ambientColor.xyz * u_ambientColor.w * baseColor * 0.25;

    // --- Sun specular (single dominant directional light) --------------
    float sunSpec = u_water_shading.y;
    int numDir = int(u_lightCounts.x);
    if (numDir > 0 && sunSpec > 0.0)
    {
        vec3 L = normalize(-u_dirLights[0].xyz);
        float intensity = u_dirLights[0].w;
        vec3 lightColor = u_dirLights[1].xyz;

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        // Tight Blinn-Phong glint for a sun streak on the waves.
        float spec = pow(NdotH, 220.0);

        float NdotL = max(dot(N, L), 0.0);
        // Soft diffuse wash so the lit side reads brighter.
        color += baseColor * lightColor * intensity * NdotL * 0.20;
        color += lightColor * intensity * spec * sunSpec;
    }

    // --- Transparency --------------------------------------------------
    // transparency 0 = opaque, 1 = fully see-through.  Push alpha up a bit at
    // grazing angles (Fresnel) so the rim reads more solid / reflective.
    float baseAlpha = clamp(1.0 - u_water_shading.x, 0.0, 1.0);
    float alpha = clamp(baseAlpha + fresnel * (1.0 - baseAlpha), 0.0, 1.0);

    // --- Output (gamma unless feeding the tonemap pass) ----------------
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));

    gl_FragColor = vec4(color, alpha);
}
