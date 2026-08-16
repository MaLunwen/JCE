$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

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

// STYLIZED overlay: shore-distance data map.  R = normalized distance from
// the waterline (0 at shore), G = in-water mask.
//
// Stage 4, NOT 5.  Stage 5 is the engine-wide s_shadowMap slot -- a program
// that puts its own texture there is fine right up until any shared bind path
// touches stage 5 for this draw, at which point either the shadows or the
// shore data are silently wrong depending on bind order.
SAMPLER2D(s_water_data, 4);
SAMPLER2D(s_cloudShadow, 3);
uniform vec4 u_cloudShadow;   // x=extent  yz=centre XZ  w=strength
#include "cloud_shadow.sh"

// ── Directional shadow receiving ───────────────────────────────────────
// Water is drawn in the translucent pass, which historically bound no shadow
// data at all -- so the sea stayed lit under a cliff that shadowed the beach
// beside it.  Nothing about that reads as a bug; it reads as water being
// bright.  These are the same uniforms and stages every opaque lit surface
// uses, so the water agrees with the terrain it meets at the shoreline.
uniform mat4 u_csmVP[4];
uniform vec4 u_csmSplits;
uniform vec4 u_csmParams;
uniform vec4 u_csmBiasScales;
uniform vec4 u_shadowQuality;
SAMPLER2D(s_csmShadow0,  9);
SAMPLER2D(s_csmShadow1, 10);
SAMPLER2D(s_csmShadow2, 11);
SAMPLER2D(s_csmShadow3, 12);

#include "csm_shadow.sh"
#include "shadow_debug.sh"

/* Rides the PBR global bind.  Declared here because water owns its own
 * program and uniform set and inherits nothing from fs_pbr_body.sh. */
uniform vec4 u_normalScale;   // z = JceSceneViewModeKind

// ── Beer-Lambert absorption ────────────────────────────────────────────
// Water does not dim light, it dims RED light -- roughly twenty times faster
// than blue, which is why deep water is blue and why a scalar extinction
// renders a swimming pool as grey haze that no surface-colour tuning fixes.
//
// This shader previously had no depth term at all: it lerped between an
// authored shallow and deep colour BY VIEW ANGLE, so a puddle and a trench
// rendered identically as long as you looked at them from the same angle.
//
// s_water_depth is the OPAQUE scene depth from the prepass target -- a
// different resource from the depth buffer being tested against, so sampling
// it here is not a read-write hazard on the bound depth attachment.
//
//   u_water_absorb.xyz = per-channel extinction (1/m), 0 = feature off
//   u_water_absorb.w   = 1 when the camera is UNDER the surface
//   u_water_absorb_tint.rgb = colour deep water converges to
//   u_water_depth_params.xy = near, far  .zw = 1/rt_width, 1/rt_height
SAMPLER2D(s_water_depth, 2);
uniform vec4 u_water_absorb;
uniform vec4 u_water_absorb_tint;
uniform vec4 u_water_depth_params;

// ── Caustics ───────────────────────────────────────────────────────────
// Not a scrolled texture.  A wave surface is a lens: where it compresses
// horizontally it focuses the sunlight through it, and the bright web on the
// floor IS that focus.  The measure of compression is the horizontal Jacobian
// of the displacement -- the same number whose NEGATIVE values define a fold,
// which is what the whitecap term already uses.  One quantity, two consumers,
// so the foam and the caustics can never drift out of step; a caustic texture
// scrolled on its own does drift, and it reads as the water moving at two
// different speeds.
//
//   u_water_caustics.x = strength (0 = off, and off is the default)
//   u_water_caustics.y = depth at which the pattern has fully faded
//   u_water_caustics.z = 1/patch size for the displacement lookup
/* Moved off stage 3 so every lit surface can read s_cloudShadow at the
 * same stage. Terrain has all sixteen stages occupied and 3 is the only
 * one it can free, so 3 is the one the others have to match. */
SAMPLER2D(s_water_caustic_disp, 6);
uniform vec4 u_water_caustics;

// ── Shoreline foam ─────────────────────────────────────────────────────
// A shoreline is a DEPTH, not a place on a texture: water is a shore wherever
// the bottom comes close to the surface.  Derived from the water column's own
// thickness -- the same `path` the absorption term computes -- so it follows a
// rock in the middle of a lake, works on a river or an L-shaped harbour, and
// needs nothing authored.
//
// The `shore_ripple` term further down measures length(v_texcoord0*2-1), the
// radius from the centre of the QUAD, and calls the rim of the mesh the shore.
// That is right for a circular pond on a square plane and wrong for anything
// else -- rings floating in open water, no foam where the water meets land.
// It is kept because scenes are authored against it, but it is not a shoreline.
//
//   u_water_shore.x = band width in METRES (0 = off).  Metres, not UV: a band
//                     in UV is a different physical width on every body, so
//                     one setting is lace on a pond and a white shelf on a sea.
//   u_water_shore.y = surge period in seconds (0 = still)
//   u_water_shore.z = foam brightness
uniform vec4 u_water_shore;

// Jacobian of the horizontal displacement, by finite difference.
// det(I + d(dxz)/d(xz)) -- the area scale of the surface at this point.
/* The Jacobian of the horizontal map, which decides where light focuses.
 *
 * It is READ, not recomputed. This function used to rebuild it from finite
 * differences of the displacement texture, and got it wrong three separate
 * ways at once:
 *
 *   1. It decoded .gb as `v * 2 - 1`, i.e. as though the channels were packed
 *      into [0,1]. They are raw metres -- the C packer writes dx[i] and dz[i]
 *      straight through into an RGBA32F, and the VERTEX shader reads them raw.
 *      The -1 happens to cancel because only DIFFERENCES are used, but the
 *      factor of two does not, so every gradient was doubled.
 *   2. It differenced per TEXEL and used the result as a derivative with
 *      respect to METRES -- missing a factor of resolution/patch_size, which
 *      for the shipping ocean is another 128/100.
 *   3. It sampled at v_texcoord0, the MESH uv, while the surface is displaced
 *      in PATCH uv (fract(worldXZ / patch_size)). For a 400 m body tiling a
 *      100 m patch those differ by four, so the pattern was not merely
 *      mis-scaled, it was drawn in the wrong place.
 *
 * None of that had to be fixed, because the correct value was already three
 * hundred lines further down being used correctly: `v_tint.x` is disp.w --
 * the Jacobian the CPU computed, sampled by the vertex shader at the right uv,
 * with the second cascade's fold already folded in by min() -- and the foam
 * term reads it and even documents it. Two consumers of one quantity, which
 * is what the header claims; there was simply a second, broken copy.
 *
 * The value is vertex-interpolated rather than per-pixel, and that is the
 * right resolution rather than a compromise: the surface being lit IS the
 * displaced mesh, so a Jacobian sampled finer than the mesh would describe a
 * surface that is not drawn. */

// Mirrors jce_water_caustics.c, and the clamp is the point: 1/jacobian
// diverges as the surface approaches a fold, and an unclamped caustic puts a
// few pixels thousands of times brighter than the scene -- which reads as
// bloom, and gets "fixed" by turning bloom down.
float water_caustic_gain(float jacobian, float strength)
{
	if (strength <= 0.0) return 1.0;
	if (jacobian <= 0.0) return 1.0;   // a fold: whitecaps handle it
	float gain = min(1.0 / jacobian, 4.0);
	return 1.0 + (gain - 1.0) * strength;
}

float water_linear_depth(float d)
{
	float n = u_water_depth_params.x;
	float f = u_water_depth_params.y;
#if BGFX_SHADER_LANGUAGE_GLSL
	float z = d * 2.0 - 1.0;
	return (2.0 * n * f) / (f + n - z * (f - n));
#else
	return (n * f) / (f - d * (f - n));
#endif
}

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
    /* Debug views, answered before anything else.
     *
     * main() has two exit points and a lot of branching between them, so the
     * only placement that covers every path is the first one. Water honoured
     * no view mode at all until now, and at the `slope` framing it is 46% of
     * the viewport -- which is why that framing spent a long time looking like
     * "geometry that ignores the debug views" when it was simply water.
     *
     * The shadow is taken from the SAME call water's lighting uses, geometric
     * normal and all, so the view reports what water actually shades with
     * rather than a second opinion. */
    {
        float dbgViewMode = u_normalScale.z;
        if (dbgViewMode > 7.5)
        {
            vec3 dbg;
            if (dbgViewMode < 8.5) {
                dbg = shadow_debug_depth_color(v_viewdepth, u_csmSplits.w);
            } else {
                float dbgCascade = -1.0;
                /* Water's own expression, verbatim -- safe_normalize_vec3
                 * lives in fs_pbr_body.sh, which this shader does not
                 * include, and the point is to report what water uses. */
                vec3 dbgL = normalize(-u_dirLights[0].xyz);
                float s = csm_shadow_factor_dbg(v_worldpos, vec3(0.0, 1.0, 0.0),
                                                dbgL, v_viewdepth, dbgCascade);
                dbg = (dbgViewMode < 9.5) ? shadow_debug_cascade_color(dbgCascade)
                                          : vec3_splat(s);
            }
            gl_FragColor = vec4(dbg, 1.0);
            return;
        }
    }

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

        // Shadow the SUN terms only -- ambient and the sky reflection are not
        // occluded by a caster between the sun and this fragment.  The normal
        // handed to the cascade lookup is the geometric surface normal, not the
        // wave-perturbed shading normal: normal-offset bias along a normal that
        // swings with every ripple makes the bias itself ripple, and the shadow
        // edge crawls across a still surface.
        float shadow = csm_shadow_factor(v_worldpos, vec3(0.0, 1.0, 0.0),
                                         L, v_viewdepth);

        /* Water under a cloud. It was the only lit surface with no cloud term
         * at all -- terrain had one, meshes had one through the SSAO pass --
         * so a cloud shadow used to stop dead at the shoreline. Multiplied
         * into the same shadow scalar the sun contribution already uses, so it
         * dims the specular sun glint and the diffuse alike and leaves the sky
         * reflection to the sky, which is already darker under its own cloud. */
        shadow *= cloud_shadow_at(v_worldpos.xz);

        // Soft diffuse wash so the lit side reads brighter.
        color += baseColor * lightColor * intensity * NdotL * 0.20 * shadow;
        color += lightColor * intensity * spec * sunSpec * shadow;
    }

    // Set by the shoreline term inside the absorption block (which is where
    // the water column's thickness is known) and consumed by the transparency
    // block below (which is where alpha is).
    float shore_foam = 0.0;

    // --- Beer-Lambert absorption ---------------------------------------
    // Applied AFTER the sun and ambient terms and BEFORE transparency: the
    // water column attenuates what is behind the surface, not the light
    // reflecting off it, so folding it into the surface shading would darken
    // the specular highlight too -- and a sun glint that dims with depth is
    // exactly the artefact that says the term is in the wrong place.
    if (u_water_absorb.x > 0.0 || u_water_absorb.y > 0.0 || u_water_absorb.z > 0.0)
    {
        vec2  duv     = gl_FragCoord.xy * u_water_depth_params.zw;
        float floor_z = water_linear_depth(texture2D(s_water_depth, duv).r);
        float surf_z  = max(v_viewdepth, 0.0);

        // Path length THROUGH WATER, not distance to the camera.  Using the
        // latter tints objects by how far away they are rather than by how much
        // water is in front of them, so a hill beside a pond is as blue as its
        // bottom.  When the camera is submerged the water starts at the eye,
        // so the path runs from 0 rather than from the surface.
        float path = (u_water_absorb.w > 0.5) ? floor_z : max(floor_z - surf_z, 0.0);

        // Caustics brighten what is BEHIND the surface, so they apply to the
        // background before the water column absorbs it -- light focused at
        // the floor still has to travel back up through the water.  Applying
        // them after absorption would make deep caustics as bright as shallow
        // ones, which is the clearest tell of a faked pattern.
        if (u_water_caustics.x > 0.0)
        {
            float jac  = v_tint.x;   /* see the note above the caustic gain */
            float gain = water_caustic_gain(jac, u_water_caustics.x);
            float t    = clamp(path / max(u_water_caustics.y, 0.001), 0.0, 1.0);
            float u    = 1.0 - t;
            float fade = u * u * (3.0 - 2.0 * u);
            color *= mix(1.0, gain, fade);
        }

        // Shoreline foam.  `path` is the water column's thickness in metres,
        // already computed above for the absorption term.
        if (u_water_shore.x > 0.0)
        {
            // The surge multiplies the BAND WIDTH, moving the effective
            // waterline in and out so the foam runs up the sand.  Modulating
            // its opacity instead would make it blink in place.
            float surge = 1.0 + 0.3 * sin(u_water_time.x
                        * (6.28318531 / max(u_water_shore.y, 0.001)));
            if (u_water_shore.y <= 0.0) surge = 1.0;
            float band = u_water_shore.x * surge;

            float f = 0.0;
            if (path < band) {
                float u = 1.0 - path / band;
                f = u * u;          // surf piles at the edge; see the CPU twin
            }

            // Break the band up so it reads as foam rather than as a contour
            // line.  Without this the term is a perfectly smooth ribbon
            // following the bathymetry, which looks like a depth visualisation.
            float n = water_vnoise2(v_worldpos.xz * 0.35
                                    + vec2(u_water_time.x * 0.08, 0.0));
            f *= smoothstep(0.15, 0.7, n) * 0.7 + 0.3;

            float a = clamp(f * u_water_shore.z, 0.0, 1.0);
            color = mix(color, vec3_splat(1.0), a);
            // Carried to the transparency block below, where alpha exists.
            // Foam must also make the surface OPAQUE: at the waterline the
            // water is at its most transparent, so white added to a nearly
            // invisible surface is nearly invisible -- the foam would be
            // brightest exactly where it shows least.
            shore_foam = a;
        }

        vec3 T = exp(-u_water_absorb.xyz * path);
        // The (1-T) in-scatter keeps deep water BLUE instead of BLACK.
        // Extinction alone drives every channel to zero, and water that goes
        // black with depth reads as a hole in the world rather than as depth.
        color = color * T + u_water_absorb_tint.rgb * (1.0 - T);
    }

    // --- Transparency --------------------------------------------------
    // transparency 0 = opaque, 1 = fully see-through.  Push alpha up a bit at
    // grazing angles (Fresnel) so the rim reads more solid / reflective.
    float baseAlpha = clamp(1.0 - u_water_shading.x, 0.0, 1.0);
    float alpha = clamp(baseAlpha + fresnel * (1.0 - baseAlpha), 0.0, 1.0);
    alpha = max(alpha, shore_foam);

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

    // --- Jacobian foam -------------------------------------------------
    //
    // v_tint.x is the determinant of the horizontal map x -> x + D(x):
    //   ~1  undisturbed        <1  compressed (a steepening face)
    //   <0  FOLDED -- which is physically what a breaking crest is.
    //
    // Whitening on that means foam appears where the surface really breaks and
    // travels with the wave, instead of being sprinkled by a noise function
    // that knows nothing about the water underneath it.  The band starts just
    // below 1 so gentle compression stays clean and only genuine steepening
    // foams.  Non-FFT modes carry 1.0 and are therefore pixel-identical.
    float foamAmt = 1.0 - smoothstep(-0.2, 0.85, v_tint.x);
    if (foamAmt > 0.001)
    {
        // Foam is a rough, bright, nearly opaque surface layer, so it lifts
        // alpha too: you cannot see through whitewater.
        vec3 foamCol = vec3(0.92, 0.95, 0.97);
        color = mix(color, foamCol, clamp(foamAmt, 0.0, 1.0) * 0.85);
        alpha = mix(alpha, 1.0, clamp(foamAmt, 0.0, 1.0) * 0.7);
    }

    // --- Output (gamma unless feeding the tonemap pass) ----------------
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));

    gl_FragColor = vec4(color, alpha);
}
