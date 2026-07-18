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
uniform vec4 u_water_shading;       // x=transparency[0..1], y=sun_specular,
                                    // z=shore_ripple, w=ice_ratio
uniform vec4 u_water_time;          // x=time (already bound for vs_water)

// IBL prefilter cube (stage 7) for an environment-tinted reflection when a
// skybox is present; falls back to a flat sky tint otherwise.
SAMPLERCUBE(s_prefilter, 7);

// STYLIZED overlay: shore-distance data map (stage 5).  R = normalized
// distance from the waterline (0 at shore), G = in-water mask.
SAMPLER2D(s_water_data, 5);

// x = water mode (0 Gerstner / 1 FFT / 2 STYLIZED), y = FFT patch size,
// z = splash ratio (STYLIZED rain circles).  Bound for vs_water already.
uniform vec4 u_water_mode;

// ── Stylized-extra helpers (shore rings / ice crackle) ──────────────────
// Fract-only hash (Hoskins) — deliberately NO sin(): the voronoi lattice is
// seeded from raw v_worldpos.xz, and sin() of a large argument loses precision
// on GL/GLES where D3D stays exact, so splash/ice patterns would diverge
// across backends far from the origin (same hazard class as fs_weather's wh21).
float water_hash12(vec2 p)
{
    vec3 q = fract(vec3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

// Cheap bilinear 2-D value noise.
float water_vnoise2(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = water_hash12(i);
    float b = water_hash12(i + vec2(1.0, 0.0));
    float c = water_hash12(i + vec2(0.0, 1.0));
    float d = water_hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// 5-octave fBM — stands in for the reference's sampled perlin_noise_256x256.png.
// The reference tiles a 256px perlin texture ~3-5x across the pond, so the ripple
// breakup carries FINE organic filaments.  Plain value-noise at the reference's
// scale 3/5 gives only a 3x3 lattice (coarse grid-aligned blobs — the "fake"
// look); stacking octaves restores perlin-like fine detail procedurally so no
// texture bind is needed.  Rotated per octave to hide the value-noise grid.
float water_fbm(vec2 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * water_vnoise2(p);
        /* rotate + scale each octave (explicit 2x2 rot, no mat2 ctor) to break
         * up the value-noise grid so stacked octaves read as organic perlin. */
        p = vec2(p.x * 0.80 + p.y * 0.60, -p.x * 0.60 + p.y * 0.80) * 2.0;
        a *= 0.5;
    }
    return v;   // ~[0,1)
}

// 2-D voronoi for the stylized splash circles / ice plates: returns
// (F1 distance, edge distance, cell id) like the reference implementation.
vec3 water_voronoi(vec2 uv)
{
    vec2 ip = floor(uv);
    vec2 fp = fract(uv);
    float f1 = 8.0, f2 = 8.0; float id = 0.0;
    for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) {
        vec2 g = vec2(float(i), float(j));
        vec2 o = vec2(water_hash12(ip + g), water_hash12(ip + g + 19.19));
        float d = length(g + o - fp);
        if (d < f1) { f2 = f1; f1 = d; id = water_hash12(ip + g + 7.7); }
        else if (d < f2) { f2 = d; }
    }
    return vec3(f1, f2 - f1, id);
}

void main()
{
    // ── STYLIZED ripple overlay (mode 2) ─────────────────────────────────
    // Hand-painted-diorama look: the water BODY is painted in the ground
    // beneath; this pass draws ONLY thin shore-hugging ripple arcs (broken
    // up by noise into brush strokes), rain splash circles, and winter ice
    // plates — every other fragment discards.  Ported from the reference
    // ripple layer with procedural value-noise standing in for its perlin
    // texture (constants preserved).
    if (u_water_mode.x > 1.5) {
        vec4  data  = texture2D(s_water_data, v_texcoord0);
        float depth = data.r;              // 0 at the shore -> 1 deep
        float mask  = data.g;              // 1 = inside the water body
        float t     = u_water_time.x;

        float finalAlpha = 0.0;
        vec3  finalColor = vec3(1.0, 1.0, 1.0);

        float shoreMask = smoothstep(0.4, 0.0, depth);

        float ripples = u_water_shading.z;     // shore_ripple ratio
        if (ripples > 0.001) {
            // The reference samples a 256px perlin TEXTURE at scale 3.0/5.0 — so
            // the breakup carries fine organic filaments.  A single value-noise
            // octave at 3/5 is only a 3x3 lattice (coarse grid blobs = the fake
            // look); water_fbm() stacks octaves to restore perlin-like detail.
            // n2 reads a rotated/offset lattice so the two are decorrelated like
            // the reference's .r/.g channels.  Scroll tracks the reference
            // (uTime ~0.06/s x uNoiseSpeed 0.5/0.3 = ~0.03 / ~0.018 UV/s).
            float n1 = water_fbm(v_texcoord0 * 3.0 + vec2(t * 0.03, 0.0));
            float n2 = water_fbm(v_texcoord0 * 5.0 + vec2(41.3, 19.7) - vec2(t * 0.018, 0.0));
            float comb = n1 * 0.6 + n2 * 0.4;
            float noisyDepth = depth + comb * 0.3;
            float ring = fract((noisyDepth + t * 0.05) * 12.0);
            float rr = smoothstep(0.0, 0.05, ring) *
                       smoothstep(0.4, 0.05, ring);
            float breakup = smoothstep(0.2, 0.75, comb);
            rr *= shoreMask * breakup;
            rr *= smoothstep(0.0, 0.1, depth);
            finalAlpha = max(finalAlpha, rr * mask * 2.5 * ripples);
        }

        float splashes = u_water_mode.z;       // splash ratio (rain)
        if (splashes > 0.001) {
            float centerMask = smoothstep(0.0, 0.5, depth);
            vec2 suv = v_worldpos.xz * 0.33;
            vec3 vor = water_voronoi(suv * 8.0);
            // Broad, LOW-frequency organic gradient (reference splashPerlin =
            // perlin at worldpos*0.0825 == suv*0.25) — gives whole regions of the
            // pond "heavier rain" instead of uniform per-cell fizz.  Feeds BOTH
            // the per-cell splash timing and its visibility, exactly like the
            // reference.  (Was suv*2.0 = 8x too fine → no spatial coherence.)
            float splashPerlin = water_vnoise2(suv * 0.25);
            // Reference uSplashesTimeFrequency 6.0 x uTime(~0.06/s) = 0.36/s ring
            // phase in real seconds (t is real seconds → t*0.36).
            float sp = fract(vor.x - (t * 0.36 +
                                      water_hash12(vec2(vor.z * 123.4, 0.0)) +
                                      splashPerlin));
            // Fatter band + lower edge gate than the raw port so each cell reads
            // as a filled irregular PATCH (reference look) rather than a thin
            // bright ring/dot.
            float thick = 0.42 * smoothstep(0.08, 0.85, vor.y);
            sp = 1.0 - step(thick, sp);
            float visible = fract(water_hash12(vec2(vor.z * 654.3, 0.0)) +
                                  splashPerlin);
            sp *= step(visible, splashes);
            sp *= centerMask;
            finalAlpha = max(finalAlpha, sp * mask);
            // Reference draws the splash rings as translucent white on the pond.
            if (sp > 0.01) finalColor = mix(finalColor, vec3_splat(1.0), sp * 0.8);
        }

        float ice = u_water_shading.w;         // ice ratio (winter)
        if (ice > 0.001) {
            vec2 iuv = v_worldpos.xz * 0.9;
            float plates = water_voronoi(iuv * 3.0).y;
            float iceMask = smoothstep(0.0, ice, depth);
            float icef = step(iceMask, plates);
            /* Softer, bluer plates than the first port: at 0.9 alpha the
             * crackle rendered as solid white cells where the reference
             * reads as translucent blue ice over the pond color. */
            finalAlpha = max(finalAlpha, icef * mask * 0.55);
            if (icef > 0.01)
                finalColor = mix(finalColor, vec3(0.72, 0.82, 0.93), icef * 0.8);
        }

        if (finalAlpha < 0.45) discard;   // reference uDiscardThreshold — crisp strokes

        // Light the strokes like the reference's MeshStandardMaterial: the white
        // ripple/splash marks are DIMMED by the scene's ambient + sun (plane
        // normal points up), so in overcast rain they read as soft grey woven
        // into the mood instead of a harsh pure white that ignores the weather.
        vec3 lit = u_ambientColor.xyz * u_ambientColor.w;
        if (int(u_lightCounts.x) > 0) {
            vec3 Ls = normalize(-u_dirLights[0].xyz);
            lit += u_dirLights[1].xyz * u_dirLights[0].w * max(Ls.y, 0.0);
        }
        finalColor *= clamp(lit, vec3_splat(0.05), vec3_splat(1.25));
        if (u_iblParams.w < 0.5)
            finalColor = pow(max(finalColor, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));
        gl_FragColor = vec4(finalColor, clamp(finalAlpha, 0.0, 1.0));
        return;
    }

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

    // --- Stylized shore ripple rings (u_water_shading.z, 0 = off) -------
    // Concentric white bands hugging the plane rim, perturbed by scrolling
    // value noise and broken up so they read hand-drawn.  The gate is
    // uniform-only, so content authored without the feature is unchanged.
    float rippleRatio = u_water_shading.z;
    if (rippleRatio > 0.001)
    {
        vec2  pc    = v_texcoord0 * 2.0 - 1.0;          // -1..1 across plane
        float r     = length(pc);                       // ~1 near the rim
        float shore = smoothstep(0.45, 0.92, r);        // rings only near shore
        float t     = u_water_time.x;
        float n1    = water_vnoise2(v_texcoord0 * 3.0 + vec2(t * 0.15,  t * 0.11));
        float n2    = water_vnoise2(v_texcoord0 * 5.0 - vec2(t * 0.09,  t * 0.13));
        float noisy = r + (n1 * 0.6 + n2 * 0.4 - 0.5) * 0.22;
        float ringT = fract(noisy * 9.0 - t * 0.55);
        float band  = smoothstep(0.0, 0.06, ringT) * (1.0 - smoothstep(0.10, 0.42, ringT));
        float breakup = smoothstep(0.25, 0.75, water_vnoise2(v_texcoord0 * 7.0 + vec2(t * 0.05, 0.0)));
        float ringA = band * shore * breakup * clamp(rippleRatio, 0.0, 1.0);
        color = mix(color, vec3_splat(1.0), clamp(ringA * 0.85, 0.0, 1.0));
        alpha = max(alpha, ringA * 0.9);
    }

    // --- Stylized ice crackle (u_water_shading.w, 0 = off) --------------
    // Winter look: blend toward a pale ice color with brighter crackle veins.
    float iceRatio = clamp(u_water_shading.w, 0.0, 1.0);
    if (iceRatio > 0.001)
    {
        float cell  = water_vnoise2(v_texcoord0 * 14.0);
        float vein  = 1.0 - smoothstep(0.0, 0.10, abs(cell - 0.5));
        vec3 iceCol = vec3(0.9, 0.95, 1.0) * (0.72 + 0.28 * vein);
        color = mix(color, iceCol, iceRatio);
        alpha = mix(alpha, 1.0, iceRatio * 0.85);
    }

    // --- Output (gamma unless feeding the tonemap pass) ----------------
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));

    gl_FragColor = vec4(color, alpha);
}
