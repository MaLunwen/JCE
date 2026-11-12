#include "water_stock_profile.sh"

#if !JCE_WATER_STYLIZED_ONLY
#include "fog_apply.sh"
#endif

#if !JCE_WATER_GL21
#include "pbr_common.sh"
#endif

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
#if JCE_WATER_GL21
#if JCE_WATER_PHYSICAL
uniform vec4 u_cameraPos;
uniform vec4 u_water_color_shallow;
uniform vec4 u_water_color_deep;
#endif
uniform vec4 u_dirLights[4];
uniform vec4 u_lightCounts;
uniform vec4 u_ambientColor;
uniform vec4 u_iblParams;
uniform vec4 u_water_shading;
uniform vec4 u_water_time;
SAMPLER2D(s_water_data, 4);
#else
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
uniform vec4 u_water_time;          // x=time, y=physical rain strength
uniform vec4 u_water_normal_detail; // low/high 1/m, slope strength, FFT phase
uniform vec4 u_weatherSurface;     // y=settled snow, bound with the water PBR state

// IBL prefilter cube (stage 7) for an environment-tinted reflection when a
// skybox is present; falls back to a flat sky tint otherwise.
SAMPLERCUBE(s_prefilter, 7);
SAMPLER2D(s_water_gradient, 14); // RG/BA: independently filtered cascade slopes
#include "water_disturbance.sh"
uniform vec4 u_water_foam; // enabled, opacity, detail scale (1/m), reserved
uniform vec4 u_water_crest[2]; // [0]: start, range, fade, strength; [1]: tint, still Y

// STYLIZED overlay: shore-distance data map.  R = normalized distance from
// the waterline (0 at shore), G = in-water mask.
//
// Stage 4, NOT 5.  Stage 5 is the engine-wide s_shadowMap slot -- a program
// that puts its own texture there is fine right up until any shared bind path
// touches stage 5 for this draw, at which point either the shadows or the
// shore data are silently wrong depending on bind order.
SAMPLER2D(s_water_data, 4);

/* PLANAR REFLECTION -- stage 8.
 *
 * Stage 8 because fs_water's free sampler slots are 0, 1, 6, 8, 13, 14, 15,
 * measured with `jce.py shader-inspect`; fs_pbr's sixteen are all taken,
 * which is why a planar reflection lands on water and not on every surface.
 *
 * The texture is LAST FRAME'S mirrored render, projected with the matrix that
 * rendered it -- u_water_planar_vp -- rather than with this frame's camera.
 * Using this frame's would be wrong by exactly the camera motion, which is
 * the artefact the one-frame delay is being honest about. */
SAMPLER2D(s_water_planar, 8);
#if defined(JCE_WATER_SCREEN_OPTICS)
SAMPLER2D(s_water_optics_resolved, 15);
uniform vec4 u_water_optics_resolve; // xy = inverse size of the trace target
#endif
uniform vec4 u_water_planar;     // x = on, y = intensity
uniform mat4 u_water_planar_vp;
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
// Per-cascade world-units-per-shadow-texel scale, used to turn a normalized
// shadow-depth difference into a penumbra width.  Written by jce_sr_shadow.c
// from JceCsmData.depth_range; see pcss.sh.
uniform vec4 u_csmPenumbra;

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

/* Stage 6 is the RGB caustic atlas for the opted-in projection. The
 * scalar fallback still reads the water vertex's signed Jacobian. */
SAMPLER2D(s_water_caustic_disp, 6);
uniform vec4 u_water_caustics;   // strength, fade depth, inverse FFT period, RGB enable
uniform vec4 u_water_caustic_map; // atlas origin, atlas extent, still-water world Y
uniform vec4 u_water_caustic_secondary; // tile origin UV, tile span, inverse secondary period
uniform vec4 u_water_caustic_detail; // weight, fade start/end (m), random grid size (m; 0 = off)
uniform vec4 u_water_caustic_fine; // tile origin UV, tile span, inverse third period
uniform vec4 u_water_caustic_fine_detail; // weight, fade end (m), random grid size (m), reserved
#include "screen_reconstruct.sh"
#include "water_cloud_layer.sh"

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

#endif

// x = water mode (0 Gerstner / 1 FFT / 2 STYLIZED), y = FFT patch size,
// z = splash ratio (STYLIZED rain circles).  Bound for vs_water already.
uniform vec4 u_water_mode;
#if !JCE_WATER_STYLIZED_ONLY
uniform vec4 u_water_params; // .z: authored Gerstner footprint
#endif

// ── Stylized-extra helpers (shore rings / ice crackle) ──────────────────
// Fract-only hash (Hoskins) — deliberately NO sin(): the voronoi lattice is
// seeded from raw v_worldpos.xz, and sin() of a large argument loses precision
// on GL/GLES where D3D stays exact, so splash/ice patterns would diverge
// across backends far from the origin (same hazard class as fs_weather's wh21).
#include "water_surface_noise.sh"

#if !JCE_WATER_GL21
vec3 water_caustic_layer_sample(vec2 phase, vec4 tile)
{
    vec2 atlasUv = tile.xy + fract(phase) * tile.z;
    // The RGB caustic atlas has one level. Explicit LOD also avoids deriving
    // gradients across the wrapped phase inside a depth-dependent branch.
    return texture2DLod(s_water_caustic_disp, atlasUv, 0.0).rgb;
}

vec2 water_caustic_vertex_offset(vec2 vertex)
{
    return vec2(water_hash12(vertex), water_hash12(vertex + 37.0));
}

vec3 water_caustic_layer_rgb(vec2 worldXZ, vec4 tile, float tileSize, bool preservePeaks)
{
    vec2 phase = worldXZ * tile.w;
    BRANCH if (tileSize > 0.0)
    {
        vec2 grid = worldXZ / tileSize;
        vec2 cell = floor(grid);
        vec2 f = fract(grid);
        float sum = f.x + f.y;
        float upper = step(1.0, sum);
        // Both triangles use the same two diagonal vertices. Each adjacent
        // cell also hashes its shared edge vertices to the same fixed offsets.
        vec2 a = cell + vec2_splat(upper);
        vec2 b = cell + vec2(1.0, 0.0);
        vec2 c = cell + vec2(0.0, 1.0);
        vec2 bc = mix(f, vec2_splat(1.0) - f.yx, upper);
        float aWeight = abs(1.0 - sum);
        if (preservePeaks) {
            // The fine tile has narrow ridges. Favor the dominant offset
            // while retaining shared-edge weights and the same three taps.
            float total = aWeight * aWeight + dot(bc, bc);
            aWeight = aWeight * aWeight / total;
            bc = bc * bc / total;
        }
        return aWeight *
                   water_caustic_layer_sample(phase + water_caustic_vertex_offset(a), tile) +
               bc.x * water_caustic_layer_sample(phase + water_caustic_vertex_offset(b), tile) +
               bc.y * water_caustic_layer_sample(phase + water_caustic_vertex_offset(c), tile);
    }
    return water_caustic_layer_sample(phase, tile);
}
#endif

// Cheap bilinear 2-D value noise.
#if !JCE_WATER_GL21
// Three shared-vertex offsets on a triangular grid. Explicit derivatives of
// the unwrapped coordinate avoid false high LOD at hash/tiling boundaries.
float water_foam_detail(vec2 uv)
{
    vec2 cell = floor(uv), f = fract(uv);
    vec2 a = cell, b = cell + vec2(1.0, 0.0), c = cell + vec2(0.0, 1.0);
    vec3 weights = vec3(1.0-f.x-f.y, f.x, f.y);
    if (f.x + f.y > 1.0) {
        a = cell + vec2(1.0, 1.0);
        b = cell + vec2(0.0, 1.0);
        c = cell + vec2(1.0, 0.0);
        weights = vec3(f.x+f.y-1.0, 1.0-f.x, 1.0-f.y);
    }
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    vec2 oa = vec2(water_hash12(a), water_hash12(a + 37.0));
    vec2 ob = vec2(water_hash12(b), water_hash12(b + 37.0));
    vec2 oc = vec2(water_hash12(c), water_hash12(c + 37.0));
    float detail = weights.x * texture2DGrad(s_water_data, uv+oa, dx, dy).b
                 + weights.y * texture2DGrad(s_water_data, uv+ob, dx, dy).b
                 + weights.z * texture2DGrad(s_water_data, uv+oc, dx, dy).b;
    return smoothstep(0.25, 0.75, detail);
}
#endif


#if !JCE_WATER_GL21


// Analytic gradient of the same interpolant keeps snow texture and normal
// registered without fetching neighbouring samples.

#include "water_froth.sh"

#include "water_surface_normal.sh"
#include "water_lighting.sh"
#endif

// 5-octave fBM — stands in for the reference's sampled perlin_noise_256x256.png.
// The reference tiles a 256px perlin texture ~3-5x across the pond, so the ripple
// breakup carries FINE organic filaments.  Plain value-noise at the reference's
// scale 3/5 gives only a 3x3 lattice (coarse grid-aligned blobs — the "fake"
// look); stacking octaves restores perlin-like fine detail procedurally so no
// texture bind is needed.  Rotated per octave to hide the value-noise grid.
#if !JCE_WATER_PHYSICAL
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

#endif

void main()
{
#if !JCE_WATER_STYLIZED_ONLY
    /* Authored Gerstner footprint, including the GL 2.1 physical program.
     * FFT history and stylized-overlay data keep their separate meanings.
     * Sample before derivative work; apply coverage at the existing end. */
    vec2 waterFootprint = vec2_splat(1.0);
    float waterFootprintBlend = 0.0;
    BRANCH if (u_water_params.z > 0.5 && u_water_mode.x < 0.5) {
        waterFootprint = texture2D(s_water_data, v_texcoord0).rg;
        waterFootprintBlend = 1.0;
    }
#endif
#if !JCE_WATER_GL21
    float shoreWeight = 0.0;
    float shoreCoverage = 1.0;
    float shoreDepth = 0.0;
    float advectedFoam = 0.0;
    float contactFoam = 0.0;
    vec2 shoreVelocity = vec2_splat(0.0);
    if (u_water_nearshore[0].w > 0.0) {
        vec2 cell = water_shore_cell(v_worldpos.xz);
        shoreWeight = water_shore_weight(cell);
        if (shoreWeight > 0.0) {
            vec4 shore = water_shore_data(cell);
            shoreDepth = max(shore.g, 0.0);
            shoreVelocity = water_shore_velocity(cell);
            shoreCoverage = mix(1.0, smoothstep(0.001, 0.01, shore.g), shoreWeight);
            advectedFoam = clamp(shore.b * u_water_nearshore[1].w * shoreWeight, 0.0, 1.0);
        }
    }

    shoreCoverage *= smoothstep(0.001, 0.01, waterFootprint.y);

    /* Debug views, after the same shoreline coverage test as colour.
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
            if (shoreCoverage < 0.01) discard;
            gl_FragColor = vec4(dbg, 1.0);
            return;
        }
    }

#endif

#if !JCE_WATER_PHYSICAL
    // ── STYLIZED ripple overlay (mode 2) ─────────────────────────────────
    // Hand-painted-diorama look: the water BODY is painted in the ground
    // beneath; this pass draws ONLY thin shore-hugging ripple arcs (broken
    // up by noise into brush strokes), rain splash circles, and winter ice
    // plates — every other fragment discards.  Ported from the reference
    // ripple layer with procedural value-noise standing in for its perlin
    // texture (constants preserved).
#if !JCE_WATER_STYLIZED_ONLY
    if (u_water_mode.x > 1.5)
#endif
    {
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
            finalColor = pow(max(finalColor, vec3_splat(0.0)), vec3_splat(u_iblParams.z));
        gl_FragColor = vec4(finalColor, clamp(finalAlpha, 0.0, 1.0));
        return;
    }

#endif
#if !JCE_WATER_STYLIZED_ONLY
#if !JCE_WATER_GL21
    WaterSurfaceNormal surfaceNormal = water_surface_normal(
        v_normal, v_worldpos, v_tangent, v_bitangent.xy, shoreDepth);
    vec3 N = surfaceNormal.normal;
    vec3 V = surfaceNormal.viewDirection;
    float iceRatio = surfaceNormal.iceRatio;
    bool fft = surfaceNormal.fft;
    bool detailedOcean = surfaceNormal.detailedOcean;
    float farDetail = surfaceNormal.farDetail;
    bool fftIce = surfaceNormal.fftIce;
    float snowOnIce = surfaceNormal.snowOnIce;
    float iceVeins = surfaceNormal.iceVeins;
    vec3 ambientIncident = water_ambient_incident(u_ambientColor);
    vec3 directIncident = water_direct_incident(u_dirLights[0], u_dirLights[1].xyz, u_lightCounts.x);
    vec3 mediumSource = water_medium_source(u_water_absorb_tint.rgb, ambientIncident, directIncident);
    vec3 iceLight = ambientIncident;
    vec3 iceGlint = vec3_splat(0.0);
#else
    vec3 N = normalize(v_normal);
    float iceRatio = clamp(u_water_shading.w, 0.0, 1.0);
    vec3 V = normalize(u_cameraPos.xyz - v_worldpos);
    if (dot(N, V) < 0.0) N = -N;
#endif

    float NdotV = clamp(dot(N, V), 0.0, 1.0);

    // --- Fresnel (Schlick) ---------------------------------------------
    // Looking straight down (high NdotV) -> shallow color shows through;
    // grazing angle (low NdotV) -> deep/reflective color dominates.
    float F0 = 0.02; // water at normal incidence ~2%
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);

    vec3 shallow = u_water_color_shallow.rgb;
    vec3 deep    = u_water_color_deep.rgb;
    float tintWeight = fresnel;
    float shorelineGrade = smoothstep(0.0, 1.0, waterFootprint.x);
    tintWeight = mix(fresnel, shorelineGrade * 0.70 + fresnel * 0.30, waterFootprintBlend);
    vec3 baseColor = mix(shallow, deep, tintWeight);

    // --- Reflection tint (environment or flat sky) ---------------------
    vec3 reflColor = vec3(0.55, 0.7, 0.9); // legacy soft-sky reflection
#if !JCE_WATER_GL21
    reflColor = water_ambient_reflection(reflect(-V, N), ambientIncident);
    if (u_iblParams.x > 0.5)
    {
        vec3 R = reflect(-V, N);
        float maxMip = u_iblParams.y;
        reflColor = textureCubeLod(s_prefilter, R, 0.15 * maxMip).rgb;
    }

#endif
    // Blend a touch of reflection in at grazing angles.
    float surfaceReflectance = fresnel * 0.6;
#if !JCE_WATER_GL21
    // Authored water colors are material response, never emitted radiance.
    // The existing direct diffuse/glint block adds its lit contribution once.
    vec3 color = mix(baseColor * ambientIncident, reflColor, surfaceReflectance);
#else
    vec3 color = mix(baseColor, reflColor, surfaceReflectance);
    color += u_ambientColor.xyz * u_ambientColor.w * baseColor * 0.25;
#endif

#if !JCE_WATER_GL21 && defined(JCE_WATER_SCREEN_OPTICS)
    vec3 opticalFallback = color;
#endif

    // --- Sun specular (single dominant directional light) --------------
    float sunSpec = u_water_shading.y;
    int numDir = int(u_lightCounts.x);
#if !JCE_WATER_GL21
    vec3 crestLight = vec3_splat(0.0);
    vec3 oceanGlint = vec3_splat(0.0);
    vec3 causticLight = vec3_splat(0.0);
    vec2 causticRaySlope = vec2_splat(0.0);
    vec3 shoreFoamLight = ambientIncident;
    float crestStrength = u_water_crest[0].w;
    bool rgbCaustics = u_water_caustics.w > 0.5;
    // Use the authored detailed FFT path, independently of normal mip sampling.
    bool shoreFilaments = u_water_params.w > 0.0 && u_water_nearshore[0].w > 0.0 &&
        u_water_nearshore[1].w > 0.0 && u_water_foam.x > 0.5 && u_water_absorb.w < 0.5;
    bool directionalCaustics = u_water_params.w > 0.0 && rgbCaustics &&
        u_water_caustic_fine_detail.x > 0.0 && u_water_absorb.w < 0.5;
    float surfaceFootprint = 0.0;
    BRANCH if (shoreFilaments || directionalCaustics || (detailedOcean && u_water_foam.x > 0.5))
        surfaceFootprint = max(length(dFdx(v_tangent.xy)), length(dFdy(v_tangent.xy)));
    bool lightShoreFoam = fft || advectedFoam > 0.0 || u_water_shore.x > 0.0;
    if (numDir > 0 && (sunSpec > 0.0 || crestStrength > 0.0 || rgbCaustics ||
                       lightShoreFoam || fftIce))
#else
    if (numDir > 0 && sunSpec > 0.0)
#endif
    {
        vec3 L = normalize(-u_dirLights[0].xyz);
        float intensity = u_dirLights[0].w;
        vec3 lightColor = u_dirLights[1].xyz;

        vec3 H = normalize(L + V);
        float NdotH = max(dot(N, H), 0.0);
        // Keep a low shoulder around the direct glint. The tighter detailed
        // lobe resolves into glints on the fine slope instead of a broad wash.
        float spec;
#if !JCE_WATER_GL21
        if (detailedOcean)
        {
            float broad = pow(clamp(NdotH, 0.0, 1.0), 84.0);
            float tight = broad * broad * broad;
            spec = mix(tight, broad, 0.08);
        }
        else
#endif
            spec = pow(NdotH, 220.0);

        float NdotL = max(dot(N, L), 0.0);

#if !JCE_WATER_GL21
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
#else
        float shadow = 1.0;
#endif

        // Soft diffuse wash so the lit side reads brighter.
        if (sunSpec > 0.0) {
            color += baseColor * lightColor * intensity * NdotL * 0.20 * shadow;
            vec3 sunGlint = lightColor * intensity * spec * sunSpec * shadow;
#if !JCE_WATER_GL21
            if (detailedOcean)
                oceanGlint = sunGlint * (smoothstep(0.0, 0.18, NdotL) *
                    mix(1.0, 0.57, shoreWeight) * farDetail);
            else
#endif
                color += sunGlint;
        }
#if !JCE_WATER_GL21
        BRANCH if (fftIce)
        {
            vec3 sunLight = lightColor * (max(intensity, 0.0) * shadow);
            iceLight += sunLight * (NdotL * 0.52);
            iceGlint = sunLight * pow(NdotH, mix(145.0, 38.0, snowOnIce)) *
                (NdotL * mix(0.28, 0.06, snowOnIce));
        }
        if (rgbCaustics) {
            causticLight = lightColor * (max(intensity, 0.0) * max(L.y, 0.0) * shadow);
            if (directionalCaustics && L.y > 0.0) {
                // Trace the refracted light ray from the receiver back to the
                // mean surface where the FFT focusing pattern was generated.
                vec3 ray = refract(-L, vec3(0.0, 1.0, 0.0), 1.0 / 1.333);
                causticRaySlope = ray.xz / -ray.y;
            }
        }
        if (lightShoreFoam)
            shoreFoamLight += lightColor * (max(intensity, 0.0) * max(L.y, 0.0) * shadow);
        if (crestStrength > 0.0) {
            float startHeight = u_water_crest[0].x;
            float crestHeight = v_worldpos.y - u_water_crest[1].w;
            float heightWeight = smoothstep(startHeight, startHeight + u_water_crest[0].y, crestHeight);
            float fadeDistance = u_water_crest[0].z;
            float distanceWeight = 1.0 - smoothstep(0.5 * fadeDistance, fadeDistance,
                length(u_cameraPos.xyz - v_worldpos));
            float backlight = clamp(0.5 - 0.5 * dot(L, V), 0.0, 1.0);
            float directionWeight = max(L.y, 0.0) * mix(0.25, 1.0, backlight);
            crestLight = u_water_crest[1].rgb * lightColor * max(intensity, 0.0) *
                (crestStrength * heightWeight * distanceWeight * directionWeight * shadow);
        }
#endif
    }

    // Set by the shoreline term inside the absorption block (which is where
    // the water column's thickness is known) and consumed by the transparency
    // block below (which is where alpha is).
    float shore_foam = 0.0;

#if !JCE_WATER_GL21
    float backgroundTransmission = 1.0;
    float horizonVisibility = 1.0;
    vec3 causticScatter = vec3_splat(0.0);
    // --- Beer-Lambert absorption ---------------------------------------
    // Apply the legacy color transfer before transparency. The detailed
    // ocean's direct glint stays separate so a deep column cannot absorb a
    // highlight formed at the surface. Optics still owns scene reflection.
    if (u_water_absorb.x > 0.0 || u_water_absorb.y > 0.0 || u_water_absorb.z > 0.0)
    {
        vec2  duv     = gl_FragCoord.xy * u_water_depth_params.zw;
        float opaqueDepth = texture2D(s_water_depth, duv).r;
        float floor_z = water_linear_depth(opaqueDepth);
        float surf_z  = max(v_viewdepth, 0.0);

        // Path length THROUGH WATER, not distance to the camera.  Using the
        // latter tints objects by how far away they are rather than by how much
        // water is in front of them, so a hill beside a pond is as blue as its
        // bottom.  When the camera is submerged the water starts at the eye,
        // so the path runs from 0 rather than from the surface.
        float path = (u_water_absorb.w > 0.5) ? floor_z : max(floor_z - surf_z, 0.0);
        if (detailedOcean && opaqueDepth >= 1.0) {
            // A clear depth sample means open water. At the camera's far
            // plane it must not turn into a thin water column over the sky.
            path = max(path, 6.0 / max(u_water_absorb.y, 0.001));
            float clipDepth = surf_z / max(u_water_depth_params.y, 1.0);
            float rim = length(v_localpos.xz) * u_water_params.w;
            // Blend the open sea into the sky before either finite boundary.
            horizonVisibility = 1.0 - max(smoothstep(0.80, 0.985, clipDepth),
                                          smoothstep(0.87, 0.995, rim));
        }

        // Caustics brighten what is BEHIND the surface, so they apply to the
        // background before the water column absorbs it -- light focused at
        // the floor still has to travel back up through the water.  Applying
        // them after absorption would make deep caustics as bright as shallow
        // ones, which is the clearest tell of a faked pattern.
        if (rgbCaustics) {
            if (opaqueDepth < 0.99999 && floor_z > surf_z) {
                vec3 receiver = jce_uv_depth_to_world(u_invViewProj, duv, opaqueDepth);
                float receiverDepth = u_water_caustic_map.w - receiver.y;
                if (receiverDepth > 0.0) {
                    float fade = 1.0 - smoothstep(0.0, u_water_caustics.y, receiverDepth);
                    vec2 focusXZ = receiver.xz - causticRaySlope * receiverDepth;
                    vec2 uv = fract(focusXZ * u_water_caustics.z);
                    vec2 atlasUv = u_water_caustic_map.xy + uv * u_water_caustic_map.z;
                    vec3 rgb = clamp(texture2D(s_water_caustic_disp, atlasUv).rgb,
                        vec3_splat(0.0), vec3_splat(3.0));
                    // One sampling body for the two optional tiles. The owner
                    // zeros each weight if its tile or depth interval is invalid.
                    bool fineActive = u_water_caustic_fine_detail.x > 0.0 &&
                                      receiverDepth < u_water_caustic_fine_detail.y;
                    LOOP for (int layer = 0; layer < 2; ++layer)
                    {
                        bool fineLayer = layer == 1;
                        if (fineLayer && !fineActive) break;
                        vec4 detailParams = fineLayer ?
                            vec4(u_water_caustic_fine_detail.x, 0.0,
                                 u_water_caustic_fine_detail.y, u_water_caustic_fine_detail.z) :
                            u_water_caustic_detail;
                        BRANCH if (detailParams.x > 0.0 && receiverDepth < detailParams.z)
                        {
                            vec4 tile = fineLayer ? u_water_caustic_fine : u_water_caustic_secondary;
                            float detail = detailParams.x * (1.0 - smoothstep(
                                detailParams.y, detailParams.z, receiverDepth));
                            if (directionalCaustics)
                                detail *= 1.0 - smoothstep(0.25, 0.75, surfaceFootprint * tile.w * 24.0);
                            BRANCH if (detail > 0.0) {
                                vec3 layerRgb = clamp(water_caustic_layer_rgb(focusXZ, tile, detailParams.w,
                                    directionalCaustics && fineLayer), vec3_splat(0.0), vec3_splat(3.0));
                                rgb = mix(rgb, layerRgb, detail);
                            }
                        }
                    }
                    if (directionalCaustics) {
                        float peak = max(rgb.r, max(rgb.g, rgb.b));
                        float energy = dot(rgb, vec3_splat(1.0 / 3.0));
                        float focus = sqrt(peak * energy) * mix(0.12, 1.85, smoothstep(0.045, 0.34, peak));
                        // Let focused energy read as warm white. Retain the
                        // smaller channel offsets as a local spectral fringe.
                        vec3 core = vec3(1.0, 0.96, 0.83) * focus;
                        rgb = min(mix(core, rgb * 0.90, 0.16), vec3_splat(3.0));
                    }
                    vec3 projectedLight = rgb * causticLight * (u_water_caustics.x * fade * (1.0 - fresnel));
                    if (directionalCaustics)
                        causticScatter = projectedLight;
                    else
                        color += projectedLight;
                }
            }
        }
        else if (u_water_caustics.x > 0.0)
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

            if (shoreFilaments && opaqueDepth < 0.99999 && floor_z > surf_z)
                contactFoam = f * shoreWeight * u_water_nearshore[1].w * u_water_shore.z * 0.70;

            // Break the band up so it reads as foam rather than as a contour
            // line.  Without this the term is a perfectly smooth ribbon
            // following the bathymetry, which looks like a depth visualisation.
            float n = water_vnoise2(v_worldpos.xz * 0.35
                                    + vec2(u_water_time.x * 0.08, 0.0));
            f *= smoothstep(0.15, 0.7, n) * 0.7 + 0.3;

            float a = clamp(f * u_water_shore.z, 0.0, 1.0) * (1.0 - shoreWeight);
            color = mix(color, min(shoreFoamLight, vec3_splat(1.10)), a);
            // Carried to the transparency block below, where alpha exists.
            // Foam must also make the surface OPAQUE: at the waterline the
            // water is at its most transparent, so white added to a nearly
            // invisible surface is nearly invisible -- the foam would be
            // brightest exactly where it shows least.
            shore_foam = a;
        }

        vec3 T = exp(-u_water_absorb.xyz * path);
        // On detailed oceans the deep column must also hide the opaque
        // background. Otherwise a finite seabed ends in a straight seam.
        if (u_water_params.w > 0.0 && u_water_absorb.w < 0.5)
            backgroundTransmission = T.y;
        // In the detailed ocean the sky lobe reflects at the interface.
        // Keep its weight in both sides of the depth mix so only the
        // transmitted part approaches the deep water color.
        vec3 extinctionLimit = mediumSource;
        if (detailedOcean)
            extinctionLimit = mix(extinctionLimit, reflColor, surfaceReflectance);
        color = color * T + extinctionLimit * (1.0 - T);
        causticScatter *= T;
#if defined(JCE_WATER_SCREEN_OPTICS)
        // The same sky lobe belongs to the fallback being replaced by a hit.
        opticalFallback = opticalFallback * T + extinctionLimit * (1.0 - T);
#endif
    }

#endif
    // --- Transparency --------------------------------------------------
    // transparency 0 = opaque, 1 = fully see-through.  Push alpha up a bit at
    // grazing angles (Fresnel) so the rim reads more solid / reflective.
    float baseAlpha = clamp(1.0 - u_water_shading.x, 0.0, 1.0);
    float alpha = clamp(baseAlpha + fresnel * (1.0 - baseAlpha), 0.0, 1.0);
#if !JCE_WATER_GL21
    if (backgroundTransmission < 1.0)
        alpha = 1.0 - (1.0 - alpha) * backgroundTransmission;
#endif
    alpha = max(alpha, shore_foam);

#if !JCE_WATER_GL21 && defined(JCE_WATER_SCREEN_OPTICS)
    // The trace resolves premultiplied optical radiance into a separate RT.
    // Keep direct light and caustics, which are outside the fallback term.
    vec2 traceUV = gl_FragCoord.xy * u_water_optics_resolve.xy;
    vec4 optics = texture2DLod(s_water_optics_resolved, traceUV, 0.0);
    float coverage = clamp(optics.a, 0.0, 1.0);
    if (coverage > 0.0)
    {
        float resolvedAlpha = alpha + (1.0 - alpha) * coverage;
        vec3 fallback = color - opticalFallback * coverage;
        color = (alpha * fallback + optics.rgb) / max(resolvedAlpha, 1e-5);
        alpha = resolvedAlpha;
    }
#endif

#if !JCE_WATER_GL21
    // Seabed light travels through the water column but should not disappear
    // with surface opacity. Convert to straight alpha with a finite gain.
    if (directionalCaustics)
        color += causticScatter * (0.85 / max(alpha, 0.25));
    if (detailedOcean)
        color += oceanGlint * ((1.0 - shore_foam) * (1.0 - iceRatio));
#endif

    // --- Stylized shore ripple rings (u_water_shading.z, 0 = off) -------
    // Concentric white bands hugging the plane rim, perturbed by scrolling
    // value noise and broken up so they read hand-drawn.  The gate is
    // uniform-only, so content authored without the feature is unchanged.
    float rippleRatio = u_water_shading.z;
#if !JCE_WATER_GL21
    // An ocean has no radial shore at the boundary of its render mesh.
    // FFT shore foam comes from depth and the advected nearshore field.
    if (fft) rippleRatio = 0.0;
#endif
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
#if !JCE_WATER_GL21
        vec3 ringColor = min(shoreFoamLight, vec3_splat(1.10));
#else
        vec3 ringColor = vec3_splat(1.0);
#endif
        color = mix(color, ringColor, clamp(ringA * 0.85, 0.0, 1.0));
        alpha = max(alpha, ringA * 0.9);
    }

    // Retain simple ice crackle on the non-FFT and GL21 paths.
#if !JCE_WATER_GL21
    if (iceRatio > 0.001 && !fftIce)
#else
    if (iceRatio > 0.001)
#endif
    {
        float cell  = water_vnoise2(v_texcoord0 * 14.0);
        float vein  = 1.0 - smoothstep(0.0, 0.10, abs(cell - 0.5));
        vec3 iceCol = vec3(0.9, 0.95, 1.0) * (0.72 + 0.28 * vein);
#if !JCE_WATER_GL21
        iceCol *= ambientIncident + directIncident;
#endif
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
#if !JCE_WATER_GL21
    if (u_water_foam.x > 0.5 && u_water_mode.x > 0.5 && u_water_mode.x < 1.5) {
        vec2 uv = v_tangent.xy / max(u_water_mode.y, 0.001);
        float coverage = texture2DGrad(s_water_data, uv, dFdx(uv), dFdy(uv)).r;
        if (u_water_mode.w > 0.0) {
            vec2 uv2 = v_tangent.xy / u_water_mode.w;
            float second = texture2DGrad(s_water_data, uv2, dFdx(uv2), dFdy(uv2)).g;
            coverage = 1.0 - (1.0-coverage)*(1.0-second);
        }
        coverage = clamp(coverage, 0.0, 1.0) * u_water_foam.y;
        if (detailedOcean) {
            foamAmt = water_froth_coverage(v_tangent.xy, u_water_foam.z,
                                           surfaceFootprint, coverage);
        } else {
            float detail = water_foam_detail(v_tangent.xy * u_water_foam.z);
            foamAmt = coverage * mix(0.35, 1.0, detail);
        }
    }
#endif
    if (foamAmt > 0.001)
    {
        // Foam is a rough, bright, nearly opaque surface layer, so it lifts
        // alpha too: you cannot see through whitewater.
        vec3 foamCol = vec3(0.92, 0.95, 0.97);
#if !JCE_WATER_GL21
        foamCol *= min(shoreFoamLight, vec3_splat(1.10));
#endif
        color = mix(color, foamCol, clamp(foamAmt, 0.0, 1.0) * 0.85);
        alpha = mix(alpha, 1.0, clamp(foamAmt, 0.0, 1.0) * 0.7);
    }

#if !JCE_WATER_GL21
    // --- Planar reflection ---------------------------------------------
    //
    // Project the world position through the matrix the MIRRORED render used
    // and sample the colour it produced.  Blended by Schlick's Fresnel over a
    // water F0 of 0.02: at a grazing angle a lake is a mirror, from straight
    // above it is nearly clear.  That is the whole reason a planar reflection
    // is worth a second scene render -- a probe cannot vary with view angle
    // and SSR has nothing above the horizon to reflect.
    if (u_water_planar.x > 0.5)
    {
        vec4 pclip = mul(u_water_planar_vp, vec4(v_worldpos, 1.0));
        if (pclip.w > 0.0)
        {
            vec2 puv = (pclip.xy / pclip.w) * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
            puv.y = 1.0 - puv.y;
#endif
            // Off the reflection target there is nothing to show; fade rather
            // than clamp, or the edge of the render becomes a hard seam
            // stretched across the water.
            vec2 pedge = abs(puv * 2.0 - 1.0);
            float pfade = 1.0 - clamp(max(pedge.x, pedge.y) * 8.0 - 7.0,
                                      0.0, 1.0);
            if (pfade > 0.0)
            {
                vec3 pv = normalize(u_cameraPos.xyz - v_worldpos);
                float pc = clamp(dot(pv, normalize(v_normal)), 0.0, 1.0);
                float pf = 0.02 + 0.98 * pow(1.0 - pc, 5.0);
                vec3 prefl = texture2D(s_water_planar, puv).rgb;
                color = mix(color, prefl,
                            clamp(pf * u_water_planar.y * pfade, 0.0, 1.0));
            }
        }
    }

#endif
#if !JCE_WATER_GL21
    // Advected foam lies on top of the reflected surface, even without
    // absorption or an opaque depth input.
    if (shoreFilaments) {
        advectedFoam = max(advectedFoam, clamp(contactFoam, 0.0, 1.0));
        BRANCH if (advectedFoam > 0.0) {
            advectedFoam = water_shore_froth(v_worldpos.xz, surfaceFootprint, advectedFoam,
                shoreVelocity, u_water_time.x, u_water_foam.z);
            shoreFoamLight = min(shoreFoamLight, vec3_splat(1.10));
        }
    }
    if (advectedFoam > 0.0) {
        vec3 foamColor = vec3(0.92, 0.95, 0.97) * shoreFoamLight;
        color = mix(color, foamColor, advectedFoam);
        alpha = max(alpha, advectedFoam);
        shore_foam = max(shore_foam, advectedFoam);
    }
    BRANCH if (fftIce)
    {
        vec3 clearIce = mix(vec3(0.10, 0.36, 0.55), vec3(0.38, 0.69, 0.83), iceVeins);
        clearIce = mix(clearIce, reflColor, fresnel * 0.42);
        vec3 snowColor = vec3(0.83, 0.90, 0.94);
        vec3 frozen = mix(clearIce, snowColor, snowOnIce) * iceLight + iceGlint;
        color = mix(color, frozen, iceRatio);
        float frozenAlpha = mix(0.60 + 0.31 * fresnel, 0.98, snowOnIce);
        alpha = mix(alpha, max(alpha, frozenAlpha), iceRatio);
    }
    // Discard after derivative-based gradient and foam reads so dry helper
    // lanes remain available at the wet/dry boundary. The discard also
    // suppresses depth writes when enabled by the material.
    if (shoreCoverage < 0.01 || horizonVisibility < 0.01) discard;
    alpha *= shoreCoverage * horizonVisibility;

    if (crestStrength > 0.0) {
        // Add the transmitted term once, after reflected colour is resolved.
        float openWater = (1.0 - clamp(foamAmt, 0.0, 1.0)) * (1.0 - shore_foam) * (1.0 - iceRatio);
        color += crestLight * ((1.0 - fresnel) * openWater);
    }
#else
    // Match the authored G-mask coverage without the modern nearshore field.
    float footprintCoverage = smoothstep(0.001, 0.01, waterFootprint.y);
    if (footprintCoverage < 0.01) discard;
    alpha *= footprintCoverage;
#endif
    // Aerial perspective from the eye to the displaced water surface.
    // The existing frame gate disables this when volumetric fog owns the pixel.
    BRANCH if (u_fogParams.x >= 0.5) {
        vec3 fogSunDir = numDir > 0 ? normalize(-u_dirLights[0].xyz) : vec3_splat(0.0);
        color = apply_aerial_fog(color, v_viewdepth, v_worldpos,
                                u_cameraPos.xyz, fogSunDir);
    }

#if !JCE_WATER_GL21
    // Complete foreground clouds cover transparent water as well as sky.
    vec4 cloud_surface = water_cloud_layer(vec4(color, alpha), v_worldpos,
                                          u_cameraPos.xyz, gl_FragCoord.xy);
    color = cloud_surface.rgb;
    alpha = cloud_surface.a;
#endif
    // --- Output (gamma unless feeding the tonemap pass) ----------------
    if (u_iblParams.w < 0.5)
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(u_iblParams.z));

    gl_FragColor = vec4(color, alpha);
#endif
}
