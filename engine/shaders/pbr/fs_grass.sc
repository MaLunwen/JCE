$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/*
 * fs_grass.sc -- standalone grass blade shading (Stage 1b.6).
 *
 * Intentionally NOT including fs_pbr_body.sh: grass owns its lighting so it
 * never touches the shared lit-body parity surface.  Reuses the PBR global
 * uniforms the scene renderer already binds (sr_inline_bind_pbr_global).
 * Wrap half-Lambert softens the terminator; a back-translucency term lets the
 * golden-hour sun glow through the tips.  Linear math, single pow at the end.
 *
 * v1 blades are solid opaque cards (no alpha-mask atlas, no discard) to avoid
 * alpha-test sorting.  An alpha-card atlas mask is a Phase-2 option.
 */
uniform vec4 u_cameraPos;     // xyz = world camera position
uniform vec4 u_dirLights[4];  // [i*2+0]=dir.xyz,intensity  [i*2+1]=color.xyz
uniform vec4 u_lightCounts;   // x = numDir
uniform vec4 u_ambientColor;  // xyz = ambient color, w = intensity
uniform vec4 u_grass_color[2];   // [0]=root.rgb, [1]=tip.rgb
// u_iblParams.w = linear output flag (1=skip gamma, for tonemap pass)
uniform vec4 u_iblParams;

void main()
{
    vec3 albedo = mix(u_grass_color[0].rgb, u_grass_color[1].rgb,
                      clamp(v_texcoord0.y, 0.0, 1.0)) * v_tint.rgb;

    vec3 N = normalize(v_normal);
    vec3 V = normalize(u_cameraPos.xyz - v_worldpos);
    if (dot(N, V) < 0.0) N = -N;   // blades viewable from both faces

    vec3 lit = u_ambientColor.rgb * u_ambientColor.w * albedo;

    int numDir = int(u_lightCounts.x);
    /* Loop bound MUST be JCE_MAX_DIR_LIGHTS (2), NOT 4: u_dirLights is declared
     * [4] (2 lights x 2 vec4) and indexed [i*2+1], so i<4 statically reaches
     * index 7 — out of bounds.  GLSL (GL backend) rejects that at compile time
     * ("array index out of bounds"), a bgfx fatal that kills all rendering;
     * HLSL/D3D silently tolerated it.  Mirrors fs_pbr_body.sh / fs_terrain.sc. */
    for (int i = 0; i < 2; ++i) {
        if (i >= numDir) break;
        vec3  L      = normalize(-u_dirLights[i*2+0].xyz);
        float inten  = u_dirLights[i*2+0].w;
        vec3  lcol   = u_dirLights[i*2+1].rgb;
        // Wrap half-Lambert (soft terminator).
        float w   = 0.5;
        float ndl = clamp((dot(N, L) + w) / (1.0 + w), 0.0, 1.0);
        // Back translucency: sun behind a thin blade glows through the tip.
        float back = pow(clamp(dot(V, -L), 0.0, 1.0), 4.0)
                     * clamp(v_texcoord0.y, 0.0, 1.0);
        lit += lcol * inten * albedo * (ndl + back * 0.6);
    }

    vec3 color = max(lit, vec3_splat(0.0));
    // When postfx tonemap is enabled, keep linear output for post-processing.
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));
    gl_FragColor = vec4(color, 1.0);
}
