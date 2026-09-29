/*
 * fs_pbr_main.sh  —  the PBR fragment program's main(), the other half of
 * fs_pbr_body.sh.  Include fs_pbr_decl.sh (and anything a variant adds) first;
 * fs_pbr_body.sh does exactly that.
 *
 * JCE_GRAPH_MATERIAL: when defined, the four places this function would fetch
 * its material from textures instead take the five values a graph-authored
 * material produced -- base colour, tangent-space normal, metallic, roughness,
 * emissive -- and EVERYTHING ELSE IS THE SAME LIGHTING.  That is the whole
 * point: a graph shader is a different material, not a different renderer.
 */
void main()
{
    // The material's texture coordinates: the mesh's UVs through this
    // material's tiling and offset.  Computed once and used by ALL FIVE maps
    // -- albedo, normal, metal-rough, AO and emissive -- because a tiling
    // that reached four of the five would read as one map being broken
    // rather than as a missing line.
    //
    // The missing-texture CHECKER below keeps using v_texcoord0 RAW, on
    // purpose: it visualises the mesh's own uv layout to diagnose unwrapping,
    // and putting the material's tiling through it would hide the very thing
    // it exists to show.
    vec2 mat_uv = v_texcoord0 * u_uvTransform.xy + u_uvTransform.zw;

#ifdef JCE_GRAPH_MATERIAL
    /* The graph produces the material; this file lights it.
     *
     * jce_graph_material is defined by the generated .sc between the
     * declarations and this function -- it can read every sampler and uniform
     * fs_pbr_decl.sh declares, and HLSL sees it before it is used. */
    vec4  jce_g_base;
    vec3  jce_g_normal_ts;
    float jce_g_metallic;
    float jce_g_roughness;
    vec3  jce_g_emissive;
    /* gl_FragCoord is the entry point's, not a helper function's -- read it
     * here and hand the result over. */
    vec2 jce_g_screen_uv = gl_FragCoord.xy / max(u_viewRect.zw, vec2_splat(1.0));
    jce_graph_material(mat_uv, v_worldpos, v_normal, jce_g_screen_uv,
                       jce_g_base, jce_g_normal_ts,
                       jce_g_metallic, jce_g_roughness, jce_g_emissive);
#endif
#ifdef JCE_FADE_DITHER
    /* LOD cross-fade screen-door (千万 ②): v_tint.a = +f (primary band copy,
     * keep where ign < f) / -f (next-band complement, keep where ign >= f) /
     * 1.0 (solid — the common fast path, no discard).  Interleaved gradient
     * noise gives the two copies EXACTLY complementary pixel subsets, so a
     * band transition dissolves instead of popping.  Compiled ONLY into
     * fs_pbr_fade (vs_pbr_inst_fade pair); every other pbr program is
     * byte-identical. */
    {
        float _fade = v_tint.a;
        if (_fade < 0.999) {
            float _ign = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x
                                                + 0.00583715 * gl_FragCoord.y));
            bool _keep = (_fade >= 0.0) ? (_ign < _fade) : (_ign >= -_fade);
            if (!_keep) discard;
        }
    }
#endif
    // --- Shadow calculation ---
    float shadow = 1.0;
    /* Which cascade this fragment was shadowed by, for view mode 9.  Function
     * scope because `cascade` below lives inside the shadow block and the
     * debug branch is further down; -1 means no cascade covered it, which is
     * a different reason to be lit than "nothing occluded it". */
    float dbgCascade = -1.0;
    float shadowDirSlot = u_lightCounts.w;
    int shadowDirIndex = int(clamp(shadowDirSlot - 1.0, 0.0, 1.0));
    vec3 toLightDir = safe_normalize_vec3(-u_dirLights[shadowDirIndex * 2].xyz,
                                          vec3(0.0, 1.0, 0.0));
    vec3 baseNormal = normalize(v_normal);

    if (u_normalScale.y > 0.0)
    {
        // Two-sided: orient the shading normal toward the viewer with a
        // half-space test against the view ray.  gl_FrontFacing here is
        // winding- AND backend-dependent (bgfx's default front face + D3D
        // SV_IsFrontFace disagree with GL for identical content): an
        // up-facing double-sided quad passed !gl_FrontFacing on its VISIBLE
        // side, shaded as its back face (N flipped away from the sun) and
        // rendered dark regardless of texture or lights.
        vec3 dsToCam = u_cameraPos.xyz - v_worldpos;
        if (dot(baseNormal, dsToCam) < 0.0)
            baseNormal = -baseNormal;
    }

    /* THE CASCADE BLOCK, and the one axis worth compiling out.
     *
     * MEASURED with `python scripts/jce.py shader-inspect`, not assumed:
     * fs_pbr's fragment program is 8311 instructions and this block is 6766
     * of them -- 81.4%.  Point lights are 276, area lights 252,
     * clearcoat+sheen 177, IBL 134, the debug views 70.  FOUR FIFTHS OF THE
     * SHADER IS THIS ONE BLOCK, which is why a Unity-style pile of material
     * keywords would have been spent on the other 18.6% and why this is the
     * only keyword axis the manifest declares for it.
     *
     * NO #else IS NEEDED: `shadow` is 1.0 above, and the prologue the rest of
     * the shader consumes -- baseNormal, toLightDir, dbgCascade -- is outside
     * the guard on purpose.  A material that does not receive shadows gets a
     * program that never had the taps, instead of one that computes them and
     * multiplies by one.
     *
     * The guard and the entry files that set it are generated from
     * contracts/shader-keywords.json; nothing here is hand-enumerated. */
#ifndef JCE_NO_CSM
    bool shadowEnabled = (shadowDirSlot > 0.5) &&
        ((u_csmSplits.x > 0.0) || (u_csmParams.x > 0.0)) &&
        (u_normalScale.w < 0.5);   /* per-renderer Receive Shadows off */

    if (shadowEnabled && u_csmSplits.x > 0.0)
    {
        float fragDepth = max(v_viewdepth, 0.0);

        int cascade = 3;
        if (fragDepth < u_csmSplits.x)      cascade = 0;
        else if (fragDepth < u_csmSplits.y)  cascade = 1;
        else if (fragDepth < u_csmSplits.z)  cascade = 2;

        int sel_cascade = cascade;
        shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir);

        // Cascade FALLTHROUGH: view-depth buckets a fragment into a cascade, but
        // the depth slab and the cascade's light-space square are differently
        // shaped, so a fragment can land just outside its bucket's square
        // (sample returns <0).  Step out to wider/farther cascades so a
        // shadow-receiving fragment is never wrongly left fully lit — that hard
        // out-of-square boundary was the triangular bright wedge ("阴影三角").
        if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir); }
        if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir); }
        if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, v_worldpos, baseNormal, toLightDir); }

        // Cascade-boundary blend — only when the depth-selected cascade actually
        // covered the fragment (skip after a fallthrough, where the split range
        // no longer matches the cascade we ended up sampling).
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
                    // Only blend toward a neighbour that actually covers it.
                    if (next_shadow >= 0.0)
                        shadow = mix(shadow, next_shadow, blend);
                }
            }
        }

        // Cascade SQUARE-EDGE blend (complements the depth-split blend above,
        // which only helps along the view-depth axis): with a coplanar
        // mega-caster (e.g. a flat ground plane toggled to cast shadows),
        // each cascade's bias balances its own texel size, but that balance
        // is DISCONTINUOUS across the light-space square's XY edge — a
        // straight knife-line step across receivers at every cascade seam.
        // Cross-fade to the next cascade over the outer 8% of the square.
        if (shadow >= 0.0 && cascade < 3)
        {
            vec4 edgeClip = csm_clip_for_cascade(cascade, v_worldpos);
            vec2 edgeUv   = edgeClip.xy / edgeClip.w * 0.5 + 0.5;
            float sqEdge  = min(min(edgeUv.x, 1.0 - edgeUv.x),
                                min(edgeUv.y, 1.0 - edgeUv.y));
            if (sqEdge < 0.08)
            {
                float edge_shadow = sample_csm_shadow(cascade + 1,
                                                      v_worldpos,
                                                      baseNormal,
                                                      toLightDir);
                if (edge_shadow >= 0.0)
                    shadow = mix(edge_shadow, shadow,
                                 clamp(sqEdge / 0.08, 0.0, 1.0));
            }
        }

        // No cascade covers this fragment (beyond the shadow range): fully lit.
        dbgCascade = (shadow < 0.0) ? -1.0 : float(cascade);
        if (shadow < 0.0) shadow = 1.0;

        // SHADOW-DISTANCE FADE: smoothly fade shadow -> lit as the fragment
        // approaches the shadow far plane (u_csmSplits.w), so the edge where
        // shadows stop is a soft gradient instead of a hard boundary that sweeps
        // across surfaces while zooming (the "会明暗" brightness flicker).
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
#endif  /* JCE_NO_CSM */

    // --- Contact shadows -----------------------------------------------
    // A short screen-space raymarch, computed in the SSAO pass and delivered in
    // the GREEN channel of the same target (.r is ambient occlusion).  It has
    // no sampler of its own because there is no free stage: all 16 are taken,
    // which is the WebGL2 budget the charter requires.
    //
    // It multiplies the DIRECTIONAL shadow term and nothing else.  Contact
    // shadows answer a specific question -- "is the sun actually reaching this
    // point, at a scale finer than the shadow map can resolve" -- and folding
    // them into ambient or into local lights would darken surfaces for a reason
    // that does not apply to those lights.  The result would still look like
    // shading rather than like a bug.
    //
    // u_ssaoParams.w > 0.5 means the SSAO pass ran WITH the march enabled.  It
    // is a separate flag from u_ssaoParams.x deliberately: the pass can be
    // running with the march off (LOW tier), and reading green in that case
    // would multiply by whatever the AO write left there.
    if (u_ssaoParams.w > 0.5)
    {
        vec2 csUV = gl_FragCoord.xy * u_ssaoParams.yz;
        shadow = min(shadow, texture2D(s_aoMap, csUV).g);
    }

    // --- Cloud shadows ---------------------------------------------------
    // Blue channel of the same target (.r AO, .g contact, .b cloud).  A
    // MULTIPLY rather than a min(): cloud transmittance and the shadow map
    // answer different questions -- "how much sun got through the cloud layer"
    // and "is a solid object in the way" -- and both apply.  min() would let a
    // half-shadowed cloud completely mask a hard shadow underneath it.
    //
    // Gated on u_ssaoParams.x (the pass ran) rather than .w (the contact march
    // ran): the cloud lookup is independent of the march, and tying them would
    // silently drop cloud shadows on the LOW tier where the march is off.
    if (u_ssaoParams.x > 0.5)
    {
        vec2 cloudUV = gl_FragCoord.xy * u_ssaoParams.yz;
        shadow *= texture2D(s_aoMap, cloudUV).b;
    }

    // --- Base color ---
    // The albedo texture is sRGB-ENCODED (glTF spec §5.19) and the SAMPLER
    // decodes it: the texture carries a hardware sRGB view (JCE_TEX_SRGB,
    // set for s_albedo and s_emissive only).  There is no pow(2.2) here any
    // more, and putting one back double-decodes.
    //
    // It moved because the decode has to happen BEFORE filtering.  Bilinear
    // taps and every mip level are averaged by fixed-function hardware, and
    // averaging is a linear-space operation -- doing it on encoded values is
    // simply the wrong answer, worst where a texture is minified, which is
    // most of the frame.  The shader could never have run first.  The sRGB
    // view also uses the real transfer function, whose linear toe below
    // 0.04045 pow(2.2) does not have.
#ifdef JCE_GRAPH_MATERIAL
    /* No albedo fetch here: whether this material samples a texture at all is
     * the graph's decision, and it already made it. */
    vec4 texColor = jce_g_base;
#elif defined(JCE_TEX_ARRAY)
    // Texture-diverse instancing: the vertex shader packs this instance's albedo
    // 2D-array layer into v_tint.x (the array variant carries no colour tint).
    vec4 texColor = texture2DArray(s_albedo, vec3(mat_uv, v_tint.x));
#else
    vec4 texColor = texture2D(s_albedo, mat_uv);
#endif
    bool useCheckerFallback = (u_normalScale.x < 0.0);
    if (useCheckerFallback)
    {
        texColor = vec4(missing_texture_checker(v_localpos, v_texcoord0), 1.0);
    }

    // One branch now: the checker fallback was the only case that already
    // arrived linear, and every case does.
    /* The factor is NOT applied to a graph's output.  compile_to_material
     * writes the graph's evaluated base colour into u_baseColorFactor so the
     * .mat.json still describes the material for anything that reads it
     * without the program (thumbnails, the inspector, a fallback); applying it
     * here as well would square it. */
#ifdef JCE_GRAPH_MATERIAL
    vec3 albedo = clamp(texColor.rgb, vec3_splat(0.0), vec3_splat(1.0));
    float alpha = texColor.a;
#else
    vec3 albedo = clamp(texColor.rgb, vec3_splat(0.0), vec3_splat(1.0))
                * u_baseColorFactor.rgb;
    float alpha = texColor.a * u_baseColorFactor.a;
#endif

#ifdef JCE_INST_TINT
    /* Per-instance tint (large-world-opt P1 #7).  v_tint carries the entity's
     * baseColor RGBA; multiplying here is the SAME operation a solo draw applies
     * via u_baseColorFactor (the bound material for a batched run is the model's
     * embedded material, so albedo = model_base * entity_tint == the solo
     * override material's base color when that override only diverges in color).
     * Tints are authored linear (like base_color_factor), so no gamma applied. */
    albedo *= v_tint.rgb;
    alpha  *= v_tint.a;
#endif

    // --- View-mode dispatch ---------------------------------------------
    // u_normalScale.z carries JceSceneViewModeKind:
    //   0 SHADED              → fall through to full PBR lighting below
    //   1 WIREFRAME           → handled by host (no fill); shader path
    //                            is unused for plain-wireframe entities
    //   2 TEXTURED            → unlit albedo-only: emit gamma-corrected
    //                            raw albedo and return BEFORE lighting,
    //                            so the user can inspect base-color
    //                            textures / UVs independent of lights
    //   3 WIREFRAME_TEXTURED  → same unlit-albedo branch (host overlays
    //                            the wireframe pass)
    // All view modes load textures; missing albedo is shown as the
    // UV-space pink/black checker via useCheckerFallback above (never
    // a flat white loading surface).
    float viewMode = u_normalScale.z;

    if (viewMode > 1.5 && viewMode < 3.5)
    {
        // Modes 2/3 (TEXTURED / WIREFRAME_TEXTURED): unlit albedo only.
        // Output albedo without lighting (gamma-correct for display).
        // Streaming LOD cross-fade screen-door (no-op unless activated).
        if (jce_lod_fade_discard(gl_FragCoord.xy)) discard;
        vec3 outRgb = pow(max(albedo, vec3_splat(0.0)), vec3_splat(u_iblParams.z));
        gl_FragColor = vec4(outRgb, alpha);
        return;
    }
    // Debug channel views (4+) fall through to after the material values
    // (normal/metallic/roughness/ao) are computed, then emit one channel.

    // --- Alpha mode ---
    float alphaMode = u_emissiveFactor.w;
    float alphaCutoff = u_pbrParams.w;

    // Mask mode: discard fragments below cutoff
    if (alphaMode == 1.0 && alpha < alphaCutoff)
    {
        discard;
    }

    // --- Normal ---
    vec3 N = baseNormal;
    float normalScale = abs(u_normalScale.x);

    // Perturb normal from normal map
    if (normalScale > 0.0)
    {
#ifdef JCE_GRAPH_MATERIAL
        vec3 tangentNormal = jce_g_normal_ts;
#else
        vec3 tangentNormal = texture2D(s_normalMap, mat_uv).xyz * 2.0 - vec3_splat(1.0);
#endif
        tangentNormal.xy *= normalScale;
        tangentNormal = normalize(tangentNormal);
        // Tangent-space -> world: columns of the basis matrix are T/B/N and
        // the tangent-space vector multiplies from the RIGHT. This MUST go
        // through mtxFromCols + mul(mtx, vec): a raw `mat3(T,B,N)` ctor is
        // row-major on HLSL but column-major on GLSL, so the previous
        // `mul(tangentNormal, mat3(T,B,N))` applied the TRANSPOSED (inverse)
        // basis on OpenGL — every normal-mapped surface got world->tangent
        // instead of tangent->world normals there (ground normals pointed
        // sideways, so every light pool was cut in half at the light's own
        // axis: N.L flipped sign with the fragment's side). D3D was correct;
        // this form is bit-identical to the old one on D3D and only fixes GL.
        /* A mesh whose vertex layout has no TANGENT still runs this branch:
         * normalScale defaults to 1.0 and does not know whether a normal map
         * was ever bound.  The missing attribute reads back as (0,0,0,1), so
         * normalize(v_tangent) is normalize(vec3(0)) and the bitangent is a
         * cross product with it -- NaN.  Even though tangentNormal is (0,0,1)
         * for the flat fallback map, and the T and B columns are therefore
         * multiplied by zero, 0 * NaN is NaN: N came out NaN and every lit
         * term collapsed to black.
         *
         * That is what made the space demo's Earth -- a procedural sphere,
         * position+normal+texcoord only -- render black on WebGL2 while the
         * glTF models beside it, which do carry tangents, lit correctly.
         * D3D and desktop GL happened to survive it; relying on that is
         * relying on undefined attribute values.
         *
         * Use the tangent basis only when there IS one. */
        vec3 Tin = v_tangent;
        if (dot(Tin, Tin) > 1e-8) {
            mat3 TBN = mtxFromCols(normalize(Tin), normalize(v_bitangent), N);
            N = normalize(mul(TBN, tangentNormal));
        }
    }

    // --- Metallic / Roughness ---
#ifdef JCE_GRAPH_MATERIAL
    float metallic  = jce_g_metallic;
    float roughness = jce_g_roughness;
#else
    vec4 mrSample = texture2D(s_metalRough, mat_uv);
    float metallic  = mrSample.b * u_pbrParams.x;
    float roughness = mrSample.g * u_pbrParams.y;
#endif

    /* Rain, same rule the terrain uses -- one include, so the ground and the
     * things standing on it cannot disagree about what wet means. */
    float wet = u_weatherSurface.x * wetness_exposure(N);
    roughness = wetness_roughness(roughness, wet);
    albedo    = wetness_albedo(albedo, wet);

    /* Lying snow, AFTER the wet response: a surface gets wet first and then
     * snow settles on top of it, and the snow is what you see. Doing it the
     * other way round darkens the snow with the rain that fell before it. */
    float snow = snow_coverage(N, u_weatherSurface.y);
    albedo     = snow_albedo(albedo, snow);
    roughness  = clamp(snow_roughness(roughness, snow), 0.04, 1.0);
    roughness = clamp(roughness, 0.04, 1.0);

    // Low-cost specular AA from normal derivatives (Toksvig-like).
    vec3 dndx = dFdx(N);
    vec3 dndy = dFdy(N);
    float normal_variance = clamp(max(dot(dndx, dndx), dot(dndy, dndy)), 0.0, 1.0);
    float aa_roughness = sqrt(normal_variance * 0.5);
    roughness = clamp(max(roughness, aa_roughness), 0.04, 1.0);

    // --- AO ---
    // When SSAO is active (u_ssaoParams.x > 0.5) the screen-space AO render
    // target is bound to the s_aoMap stage, sampled by SCREEN UV
    // (gl_FragCoord * 1/resolution) and used in place of the material AO map.
    // Otherwise the authored material AO map is sampled by mesh UV as before.
    // Either way `ao` multiplies ONLY the ambient/indirect term below.
    float ao;
    if (u_ssaoParams.x > 0.5)
    {
        vec2 ssaoUV = gl_FragCoord.xy * u_ssaoParams.yz;
        ao = texture2D(s_aoMap, ssaoUV).r;
    }
    else
    {
        ao = texture2D(s_aoMap, mat_uv).r;
        ao = mix(1.0, ao, u_pbrParams.z); // aoStrength blend
    }

    // --- Debug channel views (mode 4+): emit one raw material channel, unlit,
    //     after normal/metallic/roughness/ao are resolved. ---
    if (viewMode > 3.5)
    {
        if (jce_lod_fade_discard(gl_FragCoord.xy)) discard;
        vec3 dbg;
        if      (viewMode < 4.5) dbg = N * 0.5 + vec3_splat(0.5);  // 4 normals
        else if (viewMode < 5.5) dbg = vec3_splat(roughness);      // 5 roughness
        else if (viewMode < 6.5) dbg = vec3_splat(metallic);       // 6 metallic
        else if (viewMode < 7.5) dbg = vec3_splat(ao);             // 7 AO
        else if (viewMode < 8.5) dbg = shadow_debug_depth_color(v_viewdepth, u_csmSplits.w);
        else if (viewMode < 9.5) dbg = shadow_debug_cascade_color(dbgCascade);
        else                     dbg = vec3_splat(shadow);         // 10 mask
        gl_FragColor = vec4(dbg, 1.0);
        return;
    }

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

        // ── P3-E.5b: directional light cookie projection ───────────
        // Only the directional light selected by u_cookieDirParams.z
        // receives the cookie sampler bind (v1 single-bind shared
        // with spot cookies via sampler 13).  World-aligned ortho VP
        // built CPU-side around the camera position; UV outside
        // [0,1] is clamped to "no cookie" (multiplier 1.0).
        if (u_cookieDirParams.x > 0.5 && float(i) == u_cookieDirParams.z)
        {
            // P4-E.3a: when CSM is active (u_csmSplits.x > 0) cascade-0's
            // ortho VP is identical to the cookie VP — reuse it to save a
            // uniform slot and guarantee pixel-perfect agreement.
            mat4 _dirCookieVP = (u_csmSplits.x > 0.0) ? u_csmVP[0] : u_cookieDirVP;
            vec4 clipD = mul(_dirCookieVP, vec4(v_worldpos, 1.0));
            if (clipD.w > 0.0)
            {
                vec3 ndcD = clipD.xyz / clipD.w;
                vec2 uvD  = ndcD.xy * 0.5 + vec2(0.5, 0.5);
            #if BGFX_SHADER_LANGUAGE_GLSL
                uvD.y = 1.0 - uvD.y;
            #endif
                vec2 inUVd  = step(vec2_splat(0.0), uvD) * step(uvD, vec2_splat(1.0));
                float inMaskD = inUVd.x * inUVd.y;
                /* The atlas LAYER this directional light owns, which is in
                 * the light's OWN pack.  u_cookieDirParams.z is the light
                 * INDEX the single bind selected and .w is packed as nothing,
                 * so both of the globals here are the wrong number -- .z only
                 * ever agrees with the slot while the sun is light 0. */
                float _dirSlice = u_dirLights[i * 2 + 1].w;
                vec4  cookieDSample = texture2DArray(s_cookie, vec3(uvD, _dirSlice));
                vec3  cookieDRGB = mix(vec3_splat(1.0),
                                       cookieDSample.rgb,
                                       u_cookieDirParams.y * inMaskD);
                radiance *= cookieDRGB;
            }
        }

        float lightShadow = (abs(float(i) - float(shadowDirIndex)) < 0.5) ? shadow : 1.0;
#ifdef JCE_TOON
        // Soft-cel: quantize N·L into u_toonParams.x bands with a smoothstep
        // edge of width u_toonParams.y, then add a small spec glint step.
        {
            float ndl   = max(dot(N, lightDir), 0.0);
            float bands = max(u_toonParams.x, 2.0);
            float soft  = max(u_toonParams.y, 0.001);
            // stepped ramp: floor(ndl*bands)/bands, softened across each edge
            float scaled = ndl * bands;
            float lower  = floor(scaled) / bands;
            float frac   = scaled - floor(scaled);
            float celNdL = lower + smoothstep(0.5 - soft, 0.5 + soft, frac) / bands;
            celNdL = clamp(celNdL, 0.0, 1.0);
            Lo += albedo * radiance * celNdL * lightShadow;
        }
#else
        Lo += cookTorranceBRDFWrap(N, V, lightDir, F0, albedo, metallic, roughness, u_lookWrap.x) * radiance * lightShadow;
#endif
    }

#ifdef JCE_TOON
    // World-space rim added once (not per light): bright on the silhouette
    // facing away from the camera.  Additive in linear; default path skips it.
    {
        float NdotV_rim = clamp(dot(N, V), 0.0, 1.0);
        float rim = pow(1.0 - NdotV_rim, max(u_toonParams.z, 0.01));
        Lo += u_toonRimColor.xyz * (rim * u_toonParams.w);
    }
#endif

#ifdef JCE_FORWARDPLUS
    // ── Point + spot lights — Forward+ CLUSTERED path ───────────────────
    // Replace the brute-force 0..16 point loop (and pull spots into the same
    // froxel list) by lighting only the lights in this fragment's froxel.
    // The light list + per-light params come from the SINGLE combined cluster
    // texture (s_cluster) that jce_forwardplus uploaded; the froxel index is
    // derived from gl_FragCoord + v_viewdepth using formulas that mirror
    // jce_light_cluster.c bit-for-bit.  Attenuation / cone math is byte-
    // identical to the brute-force path so this is a CULLING change only.
    //
    // NOTE: this variant drops the per-spot IES profile (s_iesLut was freed
    // for s_cluster on stage 14) — documented MVP tradeoff behind the toggle.
    {
        float CX = u_clusterParams.x;
        float CY = u_clusterParams.y;

        // Screen UV in [0,1].  u_viewRect.zw = (width, height) of the active
        // view (bgfx built-in).  gl_FragCoord origin differs per backend:
        // bottom-left on GL (matches the CPU build's +Y-up NDC already), so
        // flip ONLY on D3D/VK/Metal — mirrors the V-flip used throughout the
        // shadow code above.
        vec2 fpUV = gl_FragCoord.xy / u_viewRect.zw;
#if !BGFX_SHADER_LANGUAGE_GLSL
        fpUV.y = 1.0 - fpUV.y;
#endif
        float tileX = clamp(floor(fpUV.x * CX), 0.0, CX - 1.0);
        float tileY = clamp(floor(fpUV.y * CY), 0.0, CY - 1.0);

        // Positive view-space depth for the slice (v_viewdepth already is).
        float vz = max(v_viewdepth, u_clusterParams2.z);
        float slice = fp_slice_for_view_z(vz);

        // c = (slice*CY + tileY)*CX + tileX  (MATCH jce_light_cluster.c).
        float cell = (slice * CY + tileY) * CX + tileX;

        // Grid texel: .r = offset, .g = count.
        vec4 gridTexel = fp_fetch(cell, u_clusterRegions.x);
        float offset = gridTexel.r;
        int count = int(gridTexel.g + 0.5);

        // Loop the froxel's lights.  The loop has a CONSTANT compile-time
        // ceiling (JCE_FP_MAX_PER_CELL) for GLSL-120 / ES loop-unroll safety —
        // never a uniform loop bound — and breaks early on the runtime count /
        // the actual max_per_cell (u_clusterParams2.x, ≤ the ceiling).  Keep
        // this >= the CPU's max_per_cell (jce_forwardplus_create uses 64).
        #define JCE_FP_MAX_PER_CELL 256
        int maxPer = int(u_clusterParams2.x + 0.5);
        for (int k = 0; k < JCE_FP_MAX_PER_CELL; k++)
        {
            if (k >= count || k >= maxPer) break;

            // Index list element: e = offset + k -> .r = light index li.
            vec4 idxTexel = fp_fetch(offset + float(k), u_clusterRegions.y);
            float li = idxTexel.r;

            // Light params: 4 texels at b = li*4 in the LIGHTS region.
            float b = li * 4.0;
            vec4 L0 = fp_fetch(b + 0.0, u_clusterRegions.z); // pos.xyz / range
            vec4 L1 = fp_fetch(b + 1.0, u_clusterRegions.z); // color.rgb / intensity
            vec4 L2 = fp_fetch(b + 2.0, u_clusterRegions.z); // type / spotDir.xyz
            vec4 L3 = fp_fetch(b + 3.0, u_clusterRegions.z); // innerCos / outerCos

            vec3 lightPos   = L0.xyz;
            float radius    = L0.w;
            vec3 lightColor = L1.rgb;
            float intensity = L1.w;
            float ltype     = L2.r;            // 0 = point, 1 = spot
            vec3 spotDir    = L2.gba;          // normalized (point: 0,0,0)
            float innerCos  = L3.r;
            float outerCos  = L3.g;

            // RENDERING LAYERS.  Skipped before any shading work: the mask
            // decides whether this light exists for this receiver at all, so
            // there is nothing to attenuate or cone-test first.
            //
            // SCOPE, and it is the same as the brute-force path's: this drops
            // the light's DIRECT term -- diffuse, specular and its shadow go
            // together -- and touches neither ambient, baked probes nor
            // volumetric fog, none of which have a receiver to mask against.
            // And it masks the RECEIVER, not the caster: an unreached object
            // still blocks this light and still appears in the shadow it casts
            // onto objects that ARE reached.  That is URP's behaviour.
            if (!fp_light_reaches(L3.b, L3.a, u_areaParams.y)) continue;

            vec3 toLight  = lightPos - v_worldpos;
            float dist    = length(toLight);
            vec3 lightDir = toLight / max(dist, 0.0001);

            // Distance attenuation — SAME formula as the brute-force path.
            float attenuation = clamp(1.0 - (dist * dist) / (radius * radius), 0.0, 1.0);
            attenuation *= attenuation;

            vec3 radiance = lightColor * intensity * attenuation;

            // Spot cone — SAME smoothstep(outerCos, innerCos, dot(L,-dir)).
            // Point lights (ltype < 0.5) skip the cone entirely.
            if (ltype > 0.5)
            {
                float theta = dot(lightDir, -spotDir);
                float epsilon = innerCos - outerCos;
                float cone = clamp((theta - outerCos) / max(epsilon, 0.0001), 0.0, 1.0);
                radiance *= cone;
            }

            // Local (spot/point) shadow — reuse the same atlas path.  The
            // clustered list does not carry a per-light shadow slot in v1,
            // so cluster lights are unshadowed (matches "culling change only"
            // — shadowed lights still come through the directional/CSM path).
            // (Shadow-slot threading into the cluster params is a v2 follow-up.)

            Lo += cookTorranceBRDFWrap(N, V, lightDir, F0, albedo, metallic, roughness, u_lookWrap.x) * radiance;
        }
    }
#else
    // --- Point lights (brute-force, default) ---
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

        // ── P1: local point shadow (slot in u_pointShadowSlot[i/4][i%4]) ──
        int  _grp    = i / 4;   // 0..3 for up to 16 point lights
        vec4 _pslotV = (_grp == 0) ? u_pointShadowSlot[0] :
                       (_grp == 1) ? u_pointShadowSlot[1] :
                       (_grp == 2) ? u_pointShadowSlot[2] : u_pointShadowSlot[3];
        int  _pl     = i - _grp * 4;
        float _pslotF = (_pl == 0) ? _pslotV.x :
                        (_pl == 1) ? _pslotV.y :
                        (_pl == 2) ? _pslotV.z : _pslotV.w;
        if (u_normalScale.w < 0.5) {  /* per-renderer Receive Shadows */
            // #7: prefer the omni cube shadow when this point has a cube base
            // slot (>=0); else the legacy single-downward tile.
            vec4 _cbV = (_grp == 0) ? u_pointCubeBaseSlot[0] :
                        (_grp == 1) ? u_pointCubeBaseSlot[1] :
                        (_grp == 2) ? u_pointCubeBaseSlot[2] : u_pointCubeBaseSlot[3];
            float _cbF = (_pl == 0) ? _cbV.x :
                         (_pl == 1) ? _cbV.y :
                         (_pl == 2) ? _cbV.z : _cbV.w;
            float _nd = max(dot(N, lightDir), 0.0);
            if (_cbF >= 0.0)
                radiance *= samplePointCubeShadow(int(_cbF), v_worldpos, N, lightPos, _nd);
            else
                radiance *= sampleLocalShadow(int(_pslotF), v_worldpos, N, lightPos, _nd);
        }

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

        // ── P3-E.5: light cookie projection ────────────────────────
        /* EVERY spot light with a slot, not just the one that won the
         * sampler.  u_spotLights[i*4+3].y is this light's own atlas layer and
         * 0 is the reserved white "no cookie" layer, so the test is "does
         * this light have one" rather than "is this light the chosen one". */
        if (u_spotLights[i * 4 + 3].y > 0.5)
        {
            /* THE PROJECTION IS PER LIGHT, and it is DERIVED rather than
             * uploaded.  It used to be one shared u_cookieSpotVP mat4 built
             * from whichever light won the single sampler bind, and that
             * matrix is what kept per-light cookies impossible after the
             * atlas, the layers and the per-light slot were all already
             * correct: every OTHER cookie-bearing light projected through the
             * FIRST light's frustum, landed outside [0,1], was masked to
             * white -- and a lit pool with no pattern is exactly what a light
             * with no cookie looks like.  Measured on the two-cookie fixture:
             * the second pool stayed blank while its layer held the right
             * image and its slot pointed at it.
             *
             * Deriving it costs no uniform at all, because a spot cookie
             * frustum is fully determined by data already in this light's own
             * pack: position, shine direction, outer cone.  It reproduces
             * jce_m4_perspective(2*acos(outerCos), 1, ..) * jce_m4_look_at()
             * term for term -- hence the f/s/u basis in that library's
             * right-handed order, and the same 0.99 up-axis switch. */
            vec3  _ckL   = v_worldpos - lightPos;
            float _ckFwd = dot(_ckL, spotDir);      /* was clip.w */
            if (_ckFwd > 0.0)
            {
                vec3 _ckUpRef = (abs(spotDir.y) > 0.99) ? vec3(0.0, 0.0, 1.0)
                                                        : vec3(0.0, 1.0, 0.0);
                vec3 _ckS = normalize(cross(spotDir, _ckUpRef));
                vec3 _ckU = cross(_ckS, spotDir);
                /* The C builder's clamp, kept: a degenerate or wider-than-
                 * hemisphere cone would give a zero or negative tangent and
                 * mirror the cookie instead of merely widening it. */
                float _ckHalf = clamp(acos(clamp(outerCos, -1.0, 1.0)),
                                      0.005, 1.55);
                vec2  ndc = vec2(dot(_ckL, _ckS), dot(_ckL, _ckU))
                          / (tan(_ckHalf) * _ckFwd);
                vec2  uv  = ndc * 0.5 + vec2(0.5, 0.5);
            #if BGFX_SHADER_LANGUAGE_GLSL
                // bgfx framebuffer y is flipped on GL; cookie textures
                // are authored top-left origin, mirror to match D3D.
                uv.y = 1.0 - uv.y;
            #endif
                // Clip outside the frustum.
                vec2 inUV = step(vec2_splat(0.0), uv) * step(uv, vec2_splat(1.0));
                float inMask = inUV.x * inUV.y;
                /* The atlas LAYER this spot light owns -- from the light,
                 * not from the global cookie params, which describe only
                 * whichever light used to win the single bind. */
                float _spotSlice = u_spotLights[i * 4 + 3].y;
                vec4  cookieSample = texture2DArray(s_cookie, vec3(uv, _spotSlice));
                // Blend toward white by (1 - strength) so a partial
                // strength still tints rather than fully masks.
                /* PER-LIGHT strength, from the free .z of this light's last
                 * vec4.  A global strength could only describe the one
                 * light that won the single bind -- keeping it would have
                 * been the same bug one field over. */
                vec3 cookieRGB = mix(vec3_splat(1.0),
                                     cookieSample.rgb,
                                     u_spotLights[i * 4 + 3].z * inMask);
                radiance *= cookieRGB;
            }
        }

        // ── P3-E.5: IES photometric profile ────────────────────────
        // Sample the 1-D LUT (256x1) by the angle between the light's
        // forward and the surface->light vector.
        if (u_iesParams.x > 0.5 && float(i) == u_iesParams.y)
        {
            float cosA = clamp(dot(-spotDir, lightDir), -1.0, 1.0);
            // Map [1..-1] (axis -> back) to [0..1] U.
            float u = (1.0 - cosA) * 0.5;
            float ies = texture2D(s_iesLut, vec2(u, 0.5)).r;
            radiance *= ies;
        }

        // ── P1: local spot shadow ──────────────────────────────────
        // (select the slot without dynamic vec4 indexing for ES compat)
        float _spotSlotF = (i == 0) ? u_spotShadowSlot.x :
                           (i == 1) ? u_spotShadowSlot.y :
                           (i == 2) ? u_spotShadowSlot.z : u_spotShadowSlot.w;
        if (u_normalScale.w < 0.5)   /* per-renderer Receive Shadows */
            radiance *= sampleLocalShadow(int(_spotSlotF), v_worldpos, N, lightPos,
                                          max(dot(N, lightDir), 0.0));

        Lo += cookTorranceBRDFWrap(N, V, lightDir, F0, albedo, metallic, roughness, u_lookWrap.x) * radiance;
    }
#endif // JCE_FORWARDPLUS

    // --- Ambient (IBL/probe + SH9 baked GI, or flat) ---
    // Baked GI consumption (P1-baked-gi-consume):
    //   * Reflection probe: when a local probe is bound the engine forces
    //     u_iblParams.x = 1 and overrides s_irradiance/s_prefilter with the
    //     probe cubemaps, so the split-sum IBL path below transparently
    //     samples the probe. u_giParams.y carries the probe intensity.
    //   * SH9 ambient: when u_giParams.x > 0.5 the diffuse irradiance comes
    //     from the light-probe SH9 reconstruction instead of the cubemap /
    //     flat ambient (avoids double-counting diffuse).
    bool sh9On = (u_giParams.x > 0.5);
    vec3 ambient;
    if (u_iblParams.x > 0.5)
    {
        // IBL ambient: split-sum approximation
        float NdotV_a = max(dot(N, V), 0.0);
        vec3 F_ibl = fresnelSchlickRoughness(NdotV_a, F0, roughness);
        vec3 kS_ibl = F_ibl;
        vec3 kD_ibl = (vec3_splat(1.0) - kS_ibl) * (1.0 - metallic);

        // Diffuse: SH9 baked irradiance when available, else the bound
        // irradiance cubemap (sky or reflection probe).
        vec3 irradiance = sh9On ? sh9_irradiance(N)
                                : textureCube(s_irradiance, N).rgb;
        vec3 diffuseIBL = kD_ibl * irradiance * albedo;

        // Specular: sample prefiltered env + BRDF LUT (probe-aware via the
        // engine-overridden s_prefilter binding), scaled by probe intensity.
        vec3 R = reflect(-V, N);
        // ReflectionProbe.box_projection: re-aim the cubemap lookup at where
        // the reflection ray actually leaves the probe's box, instead of
        // treating the cubemap as infinitely distant.
        if (u_giParams.w > 0.5)
        {
            vec3 c = u_giProbeBox[0].xyz;
            vec3 h = u_giProbeBox[1].xyz;
            // Slab test toward +R.  A zero component of R gives +-inf here and
            // min() discards it, which is the behaviour we want.
            vec3 t1 = ((c + h) - v_worldpos) / R;
            vec3 t2 = ((c - h) - v_worldpos) / R;
            vec3 tmax = max(t1, t2);
            float t = min(min(tmax.x, tmax.y), tmax.z);
            if (t > 0.0)
                R = (v_worldpos + R * t) - c;
        }
        float maxMipLevel = u_iblParams.y;
        float perceptual_roughness = sqrt(roughness);
        vec3 prefilteredColor = textureCubeLod(s_prefilter, R,
                               perceptual_roughness * maxMipLevel).rgb;
        // u_giParams.y is the reflection-probe intensity; it defaults to 1
        // when the engine binds it. bgfx zero-clears uniforms between draws,
        // so a draw path that never set u_giParams reads 0 here — treat any
        // non-positive value as 1.0 so the sky IBL is never accidentally
        // zeroed out.
        float probeScale = (u_giParams.y > 0.0) ? u_giParams.y : 1.0;
        prefilteredColor *= probeScale;
        vec2 brdfSample = texture2D(s_brdfLUT, vec2(NdotV_a, roughness)).rg;
        vec3 specularIBL = prefilteredColor * (F_ibl * brdfSample.x + vec3_splat(brdfSample.y));

        ambient = (diffuseIBL + specularIBL) * ao;

        // CLEARCOAT, environment half.  This is where the effect actually
        // LIVES: the punctual lobe added beside the light loops gives the
        // tight second highlight, but what reads as lacquer is the coat
        // REFLECTING the room -- a sharp environment reflection layered over a
        // rough base.  Without this, a clearcoat on an IBL-lit surface moves
        // about one grey level, which is what the first measurement showed.
        //
        // Same prefiltered cubemap at the COAT's roughness (that is the whole
        // point: the coat is smoother than what it covers), and the base is
        // attenuated by the coat's Fresnel first so the layer redirects energy
        // instead of adding it.
        if (u_pbrLobes.x > 0.0)
        {
            float ccRoughA  = clamp(u_pbrLobes.y, 0.03, 1.0);
            float ccPercept = sqrt(ccRoughA);
            vec3  ccPrefilt = textureCubeLod(s_prefilter, R,
                                  ccPercept * maxMipLevel).rgb * probeScale;
            float ccF = 0.04 + 0.96 * pow(1.0 - NdotV_a, 5.0);
            vec2  ccBrdf = texture2D(s_brdfLUT, vec2(NdotV_a, ccRoughA)).rg;
            vec3  ccIBL = ccPrefilt * (ccF * ccBrdf.x + ccBrdf.y);
            ambient *= (1.0 - u_pbrLobes.x * ccF);
            ambient += ccIBL * u_pbrLobes.x * ao;
        }
    }
    else if (sh9On)
    {
        // No IBL/probe, but baked SH9 ambient is present: diffuse-only.
        vec3 kD_sh = vec3_splat(1.0 - metallic);
        ambient = kD_sh * sh9_irradiance(N) * albedo * ao;
    }
    else
    {
        // Flat ambient (default) ── optionally blended into a two-color
        // hemisphere: sky color (existing u_ambientColor) at the top,
        // ground bounce (u_lookHemiGround.rgb) at the bottom, by world N.y.
        // hemi_enabled==0 => h-blend collapses to pure sky color =>
        // ambient == u_ambientColor.xyz * w * albedo * ao (unchanged).
        // Only reached when NEITHER IBL nor SH9 is active (no double-count).
        vec3 skyAmbient = u_ambientColor.xyz * u_ambientColor.w;
        float h = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 hemiColor = mix(u_lookHemiGround.xyz, skyAmbient, h);
        vec3 ambientColor = mix(skyAmbient, hemiColor, u_lookHemiGround.w);
        ambient = ambientColor * albedo * ao;
    }

    /* GI L1 (dynamic probes, u_giParams.z): ADD the runtime bounce estimate
     * on top of whichever ambient path ran above — the dynamic SH is a
     * single-bounce term, not a full ambient bake, so it must not replace
     * the sky/IBL contribution.  Gated: every legacy caller uploads z=0. */
    if (u_giParams.z > 0.5)
    {
        vec3 kD_dyn = vec3_splat(1.0 - metallic);
        ambient += kD_dyn * sh9_irradiance(N) * albedo * ao;
    }

    // --- World-space rim / fresnel (stylized silhouette separation) ---
    // Additive in LINEAR space, into `color` downstream (here folded into
    // `ambient` since `color = ambient + Lo + emissive` follows). Gated on
    // the LIT side (toLightDir) so the rim only appears on the sun/sky-
    // facing silhouette (BOTW separation). rim_intensity==0 => zero add.
    {
        float NdotV_r = clamp(dot(N, V), 0.0, 1.0);
        float rim = pow(1.0 - NdotV_r, u_lookWrap.y);          // u_lookWrap.y = rim_power
        rim *= smoothstep(0.0, 0.25, max(dot(N, toLightDir), 0.0));
        // Suppress look-profile rim for toon entities (u_lookWrap.w=toonFlag=1)
        // so the per-character toon rim (Lo+=u_toonRimColor*rim above) isn't doubled.
        ambient += u_lookRim.xyz * (u_lookWrap.z * rim * (1.0 - u_lookWrap.w)); // u_lookWrap.z = rim_intensity
    }


    // --- Extended lobes: clearcoat + sheen ---
    //
    // Evaluated ONCE against the PRIMARY directional light rather than inside
    // every light loop.  That is a stated approximation, not an oversight: a
    // second full specular evaluation per light would double the cost of the
    // punctual loops for an effect whose whole visual signature -- the tight
    // second highlight of a lacquer, the rim of a velvet -- comes from the key
    // light.  Unity's Complex Lit and Godot's clearcoat make the same trade.
    {
        float ccStrength = u_pbrLobes.x;
        vec3  sheenTint  = u_sheenColor.xyz;
        float anisoAmt   = u_pbrLobes2.x;
        float transAmt   = u_pbrLobes2.z;
        bool  wantCC     = ccStrength > 0.0;
        bool  wantSheen  = (sheenTint.x + sheenTint.y + sheenTint.z) > 0.0;
        bool  wantAniso  = anisoAmt > 0.0;
        bool  wantTrans  = transAmt > 0.0;
        if ((wantCC || wantSheen || wantAniso || wantTrans)
            && int(u_lightCounts.x) > 0)
        {
            vec3 Ld   = normalize(-u_dirLights[0].xyz);
            vec3 Lrad = u_dirLights[1].xyz * u_dirLights[0].w;
            float NdL = max(dot(N, Ld), 0.0);
            vec3 Hc   = normalize(V + Ld);

            if (wantCC)
            {
                // A thin dielectric layer over everything: its own GGX lobe at
                // its own roughness, with a fixed F0 of 0.04 (IOR 1.5, which is
                // what a lacquer is) and a Fresnel-weighted attenuation of what
                // is underneath -- without that attenuation the coat would ADD
                // energy at grazing angles instead of redirecting it.
                float ccRough = clamp(u_pbrLobes.y, 0.03, 1.0);
                float Dc = distributionGGX(N, Hc, ccRough);
                float Gc = geometrySmith(N, V, Ld, ccRough);
                float Fc = 0.04 + 0.96 * pow(1.0 - max(dot(Hc, V), 0.0), 5.0);
                float ccSpec = (Dc * Gc * Fc)
                             / max(4.0 * max(dot(N, V), 0.0) * NdL, 0.001);
                Lo *= (1.0 - ccStrength * Fc);
                Lo += vec3_splat(ccSpec * ccStrength * NdL) * Lrad;
            }

            if (wantSheen)
            {
                // Ashikhmin-Shirley's inverted Gaussian: bright at grazing,
                // dark head-on -- the retroreflection a fibrous surface has and
                // the one thing a GGX lobe cannot produce at any roughness.
                float sRough = clamp(u_pbrLobes.z, 0.07, 1.0);
                float NdH    = max(dot(N, Hc), 0.0);
                float inv    = 1.0 / (sRough * sRough);
                float sinT   = max(1.0 - NdH * NdH, 0.0);
                float Dsheen = (2.0 + inv) * pow(sinT, inv * 0.5) / (2.0 * PI);
                float Vsheen = 1.0 / (4.0 * (NdL + max(dot(N, V), 0.0)
                                             - NdL * max(dot(N, V), 0.0) + 1e-4));
                Lo += sheenTint * (Dsheen * Vsheen * NdL) * Lrad;
            }

            if (wantAniso)
            {
                // ANISOTROPY -- glTF KHR_materials_anisotropy.  The highlight
                // stretches along the surface's own tangent, rotated in the
                // tangent plane, which is what makes it follow a brushed
                // metal's grain or a hair card's flow rather than pointing
                // somewhere the artist did not author.
                //
                // The tangent frame is the one the normal map already used a
                // few dozen lines above: T from v_tangent, B from
                // v_bitangent, re-orthogonalised against the SHADING normal N
                // so a normal-mapped surface's anisotropy follows the mapped
                // normal and not the geometric one.
                //
                // ANISOTROPIC D WITH THE ISOTROPIC VISIBILITY TERM, on
                // purpose: the correlated anisotropic Smith term costs two
                // more length() per pixel for a difference that is invisible
                // beside the distribution's, and this shader has a stated
                // integrated-GPU baseline.  The same trade Filament documents
                // for its mobile tier.
                float aRot  = u_pbrLobes2.y * 6.28318530718;
                // A DEGENERATE TANGENT IS NOT RARE.  A procedural sphere's
                // tangents collapse at its poles and seam, and normalize(0) is
                // a NaN that spreads over the whole pixel -- which is exactly
                // the speckle the first version of this lobe produced.  The
                // normal-mapping block above guards the same way, and this
                // uses the same test rather than a second opinion about it.
                vec3  Traw  = v_tangent - N * dot(N, v_tangent);
                if (dot(Traw, Traw) <= 1e-8)
                {
                    // NO USABLE TANGENT AT THIS VERTEX.
                    //
                    // This used to be the common case rather than the corner:
                    // the standard vertex layout carried no TANGENT at all, so
                    // every non-skinned mesh reached here without one.  It
                    // does now, generated at mesh creation, so what is left is
                    // the genuine corner -- a vertex whose UV gradient is
                    // degenerate and whose generated tangent came from the
                    // normal, or a caller that supplied a zero.
                    //
                    // Same basis either way: Duff et al.'s branchless
                    // orthonormal frame, stable and with no screen-space
                    // derivative, which this shader's GLSL 1.20 floor does not
                    // guarantee.  The grain then does not follow the UV layout
                    // -- there is no usable one here -- and the rotation
                    // parameter is what steers it.
                    float sg = (N.z >= 0.0) ? 1.0 : -1.0;
                    float aa = -1.0 / (sg + N.z);
                    float bb = N.x * N.y * aa;
                    Traw = vec3(1.0 + sg * N.x * N.x * aa, sg * bb, -sg * N.x);
                }
                {
                vec3  Tn    = normalize(Traw);
                vec3  Bn    = normalize(cross(N, Tn));
                float ca    = cos(aRot);
                float sa    = sin(aRot);
                vec3  Ta    = normalize(Tn * ca + Bn * sa);
                vec3  Ba    = normalize(Bn * ca - Tn * sa);

                // ALPHA, not perceptual roughness.  distributionGGX squares its
                // argument internally (a = roughness*roughness), so the
                // anisotropic form has to split THAT.  Splitting the unsquared
                // value made the narrow axis about four times sharper than the
                // isotropic lobe it replaces and turned the highlight into
                // fireflies -- visible as speckle over the whole sphere.
                float aStr  = clamp(anisoAmt, 0.0, 1.0);
                float alpha = roughness * roughness;
                float at    = max(alpha * (1.0 + aStr), 0.002);
                float ab    = max(alpha * (1.0 - aStr), 0.002);

                float ToH   = dot(Ta, Hc);
                float BoH   = dot(Ba, Hc);
                float NoH   = max(dot(N, Hc), 0.0);
                float d     = (ToH * ToH) / (at * at)
                            + (BoH * BoH) / (ab * ab)
                            + NoH * NoH;
                float Daniso = 1.0 / max(PI * at * ab * d * d, 1e-6);

                float Gani  = geometrySmith(N, V, Ld, roughness);
                float Fani  = 0.04 + 0.96 * pow(1.0 - max(dot(Hc, V), 0.0), 5.0);
                float spec  = (Daniso * Gani * Fani)
                            / max(4.0 * max(dot(N, V), 0.0) * NdL, 0.001);

                // REPLACES the isotropic highlight rather than adding to it:
                // adding would leave a round core inside the stretched lobe,
                // which is the artefact that makes a cheap anisotropy read as
                // a bug.  The isotropic specular for THIS light is subtracted
                // back out at the same strength the anisotropic one is mixed
                // in, so aniso = 0 is exactly the untouched shader.
                float Diso  = distributionGGX(N, Hc, roughness);
                float iso   = (Diso * Gani * Fani)
                            / max(4.0 * max(dot(N, V), 0.0) * NdL, 0.001);
                Lo += vec3_splat((spec - iso) * aStr * NdL) * Lrad;
                }
            }

            if (wantTrans)
            {
                // TRANSLUCENCY -- light arriving through a thin surface.
                // Leaves, ears, wax, paper, a curtain with the sun behind it.
                //
                // NOT subsurface scattering: there is no diffusion profile and
                // no thickness map, and the name says so.  This is the
                // wrap-plus-back-transmission model Frostbite published and
                // Godot 4 uses at its cheap end -- one dot product and a pow,
                // against a separable blur that this shader's stated
                // integrated-GPU baseline cannot afford.
                //
                // The light is bent slightly INTO the surface before the
                // back-facing test, which is what stops the effect from being
                // a hard rim exactly opposite the light: without the
                // distortion term the transmission is a delta and reads as an
                // outline rather than as a glow.
                float thick = clamp(u_pbrLobes2.w, 0.0, 1.0);
                vec3  Lt    = normalize(Ld + N * 0.35);
                float back  = pow(clamp(dot(V, -Lt), 0.0, 1.0), 6.0);
                // Thin surfaces transmit more, so thickness ATTENUATES.
                float atten = 1.0 - thick * 0.75;
                Lo += u_translucencyColor.xyz * albedo
                    * (back * atten * transAmt) * Lrad;
            }
        }
    }

    // --- Area (rect) lights ---
    // After the point and spot loops for the same reason they follow the
    // directional one: every light adds into Lo and the order is arithmetic.
    // The maths is in area_light.sh, included once and shared with
    // fs_terrain.sc -- the other shader that consumes the punctual arrays --
    // so a rect light cannot light a mesh and miss the ground under it.
    {
        int numAreaLights = int(u_areaParams.x);
        for (int ai = 0; ai < JCE_MAX_AREA_LIGHTS; ai++)
        {
            if (ai >= numAreaLights) break;
            Lo += jce_area_light_contrib(ai, v_worldpos, N, V, F0, albedo,
                                         metallic, roughness, u_lookWrap.x);
        }
    }

    // --- Emissive ---
    // Emissive is sRGB-encoded too, and decoded by the sampler for the same
    // reason as the albedo above -- s_emissive carries the sRGB view.
#ifdef JCE_GRAPH_MATERIAL
    vec3 emissive = jce_g_emissive;
#else
    vec3 emissiveTex = clamp(texture2D(s_emissive, mat_uv).rgb, vec3_splat(0.0), vec3_splat(1.0));
    vec3 emissive = emissiveTex * u_emissiveFactor.xyz;
#endif

    // --- Final color ---
    vec3 color = ambient + Lo + emissive;

    // --- Aerial-perspective fog (linear, before gamma) ---
    // toLightDir is the unit dir from surface toward the shadow-casting sun
    // (computed at the top of main, line ~514). v_viewdepth is positive
    // view-space distance. No-op when u_fogParams.x < 0.5 (default).
    color = apply_aerial_fog(color, v_viewdepth, v_worldpos,
                             u_cameraPos.xyz, toLightDir);

    // --- Gamma correction (linear -> sRGB) ---
    // When postfx tonemap is enabled, keep linear output for post-processing.
    if (u_iblParams.w < 0.5)
    {
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(u_iblParams.z));

        // --- Dither (break up 8-bit banding), LDR path ONLY ---
        // With no tonemap pass following, this pass writes the final 8-bit
        // image, so a smooth radial light-falloff gradient quantizes into
        // concentric "ring" bands under point/spot lights; a ~1/255 ordered
        // dither keyed on screen position hides the steps. On the HDR path
        // (linear out to RGBA16F) dithering here is useless noise that ACES
        // then distorts — fs_composite.sc dithers at the real final
        // quantization point instead. Each path dithers exactly once.
        float _dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)))
                              * 43758.5453);
        color += vec3_splat((_dither - 0.5) / 255.0);
    }

    // --- Streaming LOD cross-fade screen-door (no-op unless activated) ---
    // Placed once before the final output so it covers both the blend
    // (alphaMode==2.0) and the opaque output branches below.
    if (jce_lod_fade_discard(gl_FragCoord.xy)) discard;

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
