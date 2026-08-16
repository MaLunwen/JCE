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

// Aerial fog (u_fogParams/u_fogColor/u_fogColorSun are in the same PBR
// global bind this shader already relies on; gate mode NONE = bit-exact
// no-op).  The reference fogs EVERY material (three.js scene.fog applies
// to grass/bush/ground alike) — without this the far grass rim stayed
// crisp against the fogged ground and the diorama edge never dissolved.
#include "fog_apply.sh"
#include "shadow_debug.sh"

/* Rides the PBR global bind, like u_cameraPos / u_dirLights below.  Declared
 * here because this shader is self-contained: it has its own main() and its
 * own copy of the cascade selection, so it inherits nothing from
 * fs_pbr_body.sh. */
uniform vec4 u_normalScale;   // z = JceSceneViewModeKind

// ── CSM receive (lite port of fs_pbr_body's sampler) ────────────────────
// The reference grass mesh is receiveShadow=true: tree/bush shadows drape
// visibly ON the carpet (and flutter with the caster's wind).  A dense
// carpet hides the ground plane, so without this the canopy shadows were
// invisible.  Lite = fixed 2x2 PCF (blades are high-frequency; full 25-tap
// rotation buys nothing here) + the SAME out-of-square sentinel & cascade
// fallthrough as fs_pbr_body — its absence (all-shadow at some camera
// distances) is what sank the previous grass-CSM attempt.  Uniform/sampler
// names and stages match sr_bind_frame_shadow_state's frame bind.
uniform mat4 u_csmVP[4];
uniform vec4 u_csmSplits;
uniform vec4 u_csmParams;      // x = inv shadow-map size, z = normal-offset
uniform vec4 u_csmBiasScales;
SAMPLER2D(s_cloudShadow, 3);
uniform vec4 u_cloudShadow;   // x=extent  yz=centre XZ  w=strength
#include "cloud_shadow.sh"
SAMPLER2D(s_csmShadow0, 9);
SAMPLER2D(s_csmShadow1, 10);
SAMPLER2D(s_csmShadow2, 11);
SAMPLER2D(s_csmShadow3, 12);

float grass_csm_depth(int cascade, vec2 uv)
{
    if      (cascade == 0) return texture2D(s_csmShadow0, uv).r;
    else if (cascade == 1) return texture2D(s_csmShadow1, uv).r;
    else if (cascade == 2) return texture2D(s_csmShadow2, uv).r;
    return texture2D(s_csmShadow3, uv).r;
}

float grass_sample_csm(int cascade, vec3 world_pos, vec3 to_light)
{
    float bias_scale = (cascade == 0) ? u_csmBiasScales.x
                     : (cascade == 1) ? u_csmBiasScales.y
                     : (cascade == 2) ? u_csmBiasScales.z : u_csmBiasScales.w;
    // The receiving surface is the flat carpet the blades stand on: bias
    // along world +Y (blade normals point sideways and would mis-bias).
    // FLOOR of 0.35 world units: when the ground plane (or any coplanar
    // surface under the blades) is set to CAST shadows, blade fragments
    // near y=0 sample the caster's own depth and z-fight into acne — the
    // crisp near-black diagonal bands that sharpen as cascade 0 resolves
    // them up close.  Lifting the sample point ~a third of a blade height
    // clears the coplanar depth; canopy shadows arrive from metres above
    // and only shift by centimetres.
    vec3 biased = world_pos
                + vec3(0.0, 1.0, 0.0)
                  * max(u_csmParams.z * bias_scale * 1.5, 0.35);

    vec4 clipP = (cascade == 0) ? mul(u_csmVP[0], vec4(biased, 1.0))
               : (cascade == 1) ? mul(u_csmVP[1], vec4(biased, 1.0))
               : (cascade == 2) ? mul(u_csmVP[2], vec4(biased, 1.0))
                                : mul(u_csmVP[3], vec4(biased, 1.0));
    vec3 ndc = clipP.xyz / clipP.w;
    vec2 uv  = ndc.xy * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
    uv.y = 1.0 - uv.y;
#endif
#if BGFX_SHADER_LANGUAGE_GLSL
    float z = ndc.z * 0.5 + 0.5;
#else
    float z = ndc.z;
#endif
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        z < 0.0 || z > 1.0)
        return -1.0;                       // sentinel: cascade doesn't cover

    float inv_map = max(u_csmParams.x, 1.0 / 2048.0);
    float clerp   = clamp(float(cascade) * (1.0 / 3.0), 0.0, 1.0);
    float ndl     = max(to_light.y, 0.1); // carpet normal = +Y
    float slope   = sqrt(max(1.0 - ndl * ndl, 0.0)) / ndl;
    float dbias   = inv_map * mix(3.0, 6.0, clerp) * bias_scale
                  * (1.0 + slope * 8.0);
    dbias = min(dbias, mix(0.01, 0.08, clerp));

    // 2x2 PCF.
    float sum = 0.0;
    for (int y = 0; y <= 1; y++)
    for (int x = 0; x <= 1; x++)
    {
        vec2 off = (vec2(float(x), float(y)) - 0.5) * inv_map * 1.5;
        sum += (z - dbias > grass_csm_depth(cascade, uv + off)) ? 0.0 : 1.0;
    }
    return sum * 0.25;
}

void main()
{
    // Reference 3-tone grass (GrassManager fragment): a dark SHADOW base ramps
    // into the dark->light body via pow(smoothstep(0.2,0.98,mask),0.5).  The
    // bottom ~20% of the blade is (near) pure shadow, grounding the tuft; the
    // tip is the bright body colour.  (uShadowColor is authored in the ref; we
    // derive it as a darkened root since the director ships only root+tip.)
    float bladeMask = clamp(v_texcoord0.y, 0.0, 1.0);
    vec3 body   = mix(u_grass_color[0].rgb, u_grass_color[1].rgb, bladeMask);
    vec3 shadow = u_grass_color[0].rgb * 0.30;
    float finalMask = pow(smoothstep(0.2, 0.98, bladeMask), 0.5);
    vec3 albedo = mix(shadow, body, finalMask) * v_tint.rgb;

    vec3 N = normalize(v_normal);
    vec3 V = normalize(u_cameraPos.xyz - v_worldpos);
    if (dot(N, V) < 0.0) N = -N;   // blades viewable from both faces

    vec3 lit = u_ambientColor.rgb * u_ambientColor.w * albedo;

    // Canopy shadow on the carpet (reference grass is receiveShadow=true).
    // Same depth-bucket selection + out-of-square fallthrough as fs_pbr_body;
    // applied only to the shadow-casting dir light below.
    float csmSh     = 1.0;
    int   shadowIdx = -1;
    /* Cascade actually used, for view mode 9 -- taken from the selection
     * below rather than recomputed, same rule as the other two shading
     * paths. */
    float dbgCascade = -1.0;
    if (u_lightCounts.w > 0.5 && u_csmSplits.x > 0.0)
    {
        shadowIdx = int(clamp(u_lightCounts.w - 1.0, 0.0, 1.0));
        vec3 toL  = normalize(-u_dirLights[shadowIdx * 2 + 0].xyz);
        float fragDepth = max(v_viewdepth, 0.0);
        int cascade = 3;
        if      (fragDepth < u_csmSplits.x) cascade = 0;
        else if (fragDepth < u_csmSplits.y) cascade = 1;
        else if (fragDepth < u_csmSplits.z) cascade = 2;
        csmSh = grass_sample_csm(cascade, v_worldpos, toL);
        if (csmSh < 0.0 && cascade < 3) { cascade = cascade + 1; csmSh = grass_sample_csm(cascade, v_worldpos, toL); }
        if (csmSh < 0.0 && cascade < 3) { cascade = cascade + 1; csmSh = grass_sample_csm(cascade, v_worldpos, toL); }
        if (csmSh < 0.0 && cascade < 3) { cascade = cascade + 1; csmSh = grass_sample_csm(cascade, v_worldpos, toL); }
        dbgCascade = (csmSh < 0.0) ? -1.0 : float(cascade);
        if (csmSh < 0.0) csmSh = 1.0;      // beyond every cascade = lit
        // Cascade square-edge blend: with a coplanar mega-caster (e.g. the
        // ground plane toggled to cast) each cascade's bias balances its own
        // texel size, but the balance is DISCONTINUOUS across the light-space
        // square edge — a straight knife-line step at every cascade seam.
        // Cross-fade to the next cascade over the outer 8% of the square.
        else if (cascade < 3) {
            vec4 clipE = (cascade == 0) ? mul(u_csmVP[0], vec4(v_worldpos, 1.0))
                       : (cascade == 1) ? mul(u_csmVP[1], vec4(v_worldpos, 1.0))
                                        : mul(u_csmVP[2], vec4(v_worldpos, 1.0));
            vec2 uvE = clipE.xy / clipE.w * 0.5 + 0.5;
            float edge = min(min(uvE.x, 1.0 - uvE.x), min(uvE.y, 1.0 - uvE.y));
            if (edge < 0.08) {
                float s2 = grass_sample_csm(cascade + 1, v_worldpos, toL);
                if (s2 >= 0.0)
                    csmSh = mix(s2, csmSh, clamp(edge / 0.08, 0.0, 1.0));
            }
        }
        // Stylized floor: canopy shade dims the carpet, never blacks it
        // (the reference keeps fill + env light in its shadowed grass).
        csmSh = mix(0.5, 1.0, csmSh);
    }

    /* A cloud overhead darkens the grass exactly as it darkens the ground the
     * grass stands in. It did not: terrain sampled the baked map and the blades
     * on it did not, so a cloud shadow crossing a field left lit grass standing
     * in a dark patch.
     *
     * AFTER the whole cascade block, not inside it. The first attempt put this
     * line between `if (csmSh < 0.0) csmSh = 1.0;` and the `else if` that
     * follows it, which severed an if/else chain -- three compilers said so,
     * in three different dialects, which is the useful half of a shader that
     * has to build for all of them.
     *
     * Folded into csmSh so it multiplies the shadow-casting directional light
     * and nothing else: a cloud does not dim the ambient sky term, it IS the
     * sky term changing, and the environment already handles that. */
    csmSh *= cloud_shadow_at(v_worldpos.xz);

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
        float sh = (i == shadowIdx) ? csmSh : 1.0;
        lit += lcol * inten * albedo * (ndl + back * 0.6) * sh;
    }

    vec3 color = max(lit, vec3_splat(0.0));

    // Boundary/aerial fog in LINEAR space, mirroring fs_pbr_body (the sun
    // warm-shift keys on the primary dir light like the lit loop above).
    vec3 fogSunDir = (numDir > 0) ? normalize(-u_dirLights[0].xyz)
                                  : vec3(0.0, 1.0, 0.0);
    color = apply_aerial_fog(color, v_viewdepth, v_worldpos,
                             u_cameraPos.xyz, fogSunDir);

    /* Debug views.  This shader honoured none of them, and in a scene whose
     * vegetation is 560 scattered grass and bush entities that meant 46% of
     * the viewport was invisible to every debug view -- including the depth
     * view a distance measurement depends on. The gap was found by the depth
     * view itself: it emits greyscale, so any coloured geometry pixel is a
     * shader that ignored it, and 46% of the frame stayed green.
     *
     * Reported from this shader's OWN cascade result. fs_grass.sc carries a
     * third independent copy of the cascade selection (after fs_pbr_body.sh
     * and csm_shadow.sh), so a view that re-derived the answer could show a
     * cascade this shader never sampled -- which is exactly the divergence
     * worth being able to see. */
    float viewMode = u_normalScale.z;
    if (viewMode > 7.5)
    {
        vec3 dbg;
        if      (viewMode < 8.5) dbg = shadow_debug_depth_color(v_viewdepth, u_csmSplits.w);
        else if (viewMode < 9.5) dbg = shadow_debug_cascade_color(dbgCascade);
        else                     dbg = vec3_splat(csmSh);
        gl_FragColor = vec4(dbg, 1.0);
        return;
    }

    // When postfx tonemap is enabled, keep linear output for post-processing.
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));
    gl_FragColor = vec4(color, 1.0);
}
