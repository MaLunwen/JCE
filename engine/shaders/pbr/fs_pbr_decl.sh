/*
 * fs_pbr_decl.sh  —  everything the PBR fragment body DECLARES: the includes,
 * the uniforms, the samplers and the helper functions, up to but not
 * including main().
 *
 * Split out of fs_pbr_body.sh so a third kind of entry point can exist
 * alongside the two that already share this source of truth.  The two are
 * variants -- fs_pbr.sc and fs_pbr_fwdplus.sc differ only by a #define before
 * the include.  The third is a graph-authored material, which needs to run
 * generated code that READS these samplers and uniforms and writes the five
 * material values main() consumes.  That code has to sit textually after the
 * declarations and before main(), because HLSL will not accept a function
 * used before it is defined -- so the file had to become two.
 *
 * fs_pbr_body.sh still exists and still includes both halves in order, so
 * fs_pbr.sc and fs_pbr_fwdplus.sc are untouched by this split.  Verified the
 * only way that claim can be verified: the compiled blobs for fs_pbr,
 * fs_pbr_fwdplus, fs_pbr_toon, fs_pbr_tint and fs_pbr_fade are byte-identical
 * across it.
 */
/*
 * fs_pbr_body.sh  —  shared fragment-shader body for the PBR material.
 *
 * This is the entire fs_pbr fragment program EXCEPT the leading `$input`
 * declaration, factored out so two thin entry-point .sc files can build two
 * compiled programs from ONE source of truth:
 *
 *   fs_pbr.sc          — `$input ...` + #include "fs_pbr_body.sh"
 *                        (the DEFAULT brute-force point-light path; ships
 *                         today; byte-identical to the historical fs_pbr.sc).
 *   fs_pbr_fwdplus.sc  — `$input ...` + #define JCE_FORWARDPLUS
 *                                       + #include "fs_pbr_body.sh"
 *                        (the OPT-IN Forward+ clustered variant: replaces the
 *                         16-light brute-force loop with a per-froxel cluster
 *                         loop; drops the IES sampler to free sampler stage 14
 *                         for the cluster data texture — WebGL2 ≤16 samplers).
 *
 * `$input` MUST stay in the .sc entry files (shaderc only strips the leading
 * $input/$output from the MAIN source, not from #include'd files) — this is
 * why the body lives in a .sh and the varyings are declared by each .sc.
 *
 * #ifdef JCE_FORWARDPLUS gates EVERY behavioural difference; with the macro
 * undefined this file is the original fs_pbr.sc verbatim (same uniforms, same
 * IES path, same brute-force loop) so the default program is unchanged.
 */

#include <bgfx_shader.sh>
#include "pbr_common.sh"
#include "fog_apply.sh"
#include "shadow_debug.sh"
uniform vec4 u_weatherSurface;  // x=wetness y=snow zw=reserved
#include "wetness.sh"
#include "area_light.sh"

// Material uniforms
uniform vec4 u_baseColorFactor;
uniform vec4 u_pbrParams;       // x=metallic, y=roughness, z=aoStrength, w=alphaCutoff
uniform vec4 u_emissiveFactor;  // xyz=emissive, w=alphaMode (0=opaque, 1=mask, 2=blend)
uniform vec4 u_cameraPos;       // xyz=world-space camera position
uniform vec4 u_normalScale;     // x=normal map scale (x<0 => checker fallback), y=doubleSided flag
uniform vec4 u_ambientColor;    // xyz=ambient color, w=ambient intensity

// Toon (cel) material uniforms — only declared/consumed under #ifdef JCE_TOON.
// Default (fs_pbr.sc / fs_pbr_fwdplus.sc) leaves JCE_TOON undefined so these
// are NOT compiled in and the default programs are byte-identical.
#ifdef JCE_TOON
uniform vec4 u_toonParams;    // x=bands(2..4), y=ramp threshold/softness, z=rim power, w=rim intensity
uniform vec4 u_toonRimColor;  // xyz=rim color (linear), w=pad
#endif

// ── Look Profile uniforms (stylized slice plan 02; consumed by plan 03/04) ──
// u_lookWrap      = {wrap_factor, rim_power, rim_intensity, toonFlag}
// u_lookRim       = {rim_color.rgb, pad}
// u_lookHemiGround= {ambient_ground_color.rgb, hemi_enabled}
// Hemisphere TOP color = u_ambientColor.  Neutral packing (wrap=0,
// rim_intensity=0, hemi_enabled=0) => the plan 03/04 math is an algebraic
// no-op.  Declared here so this plan ships the uniforms without altering
// any backend output (verified byte-identical via render_parity).
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
#ifdef JCE_TEX_ARRAY
// Texture-diverse instancing: the albedo slot is a 2D-array; each instance
// samples its own layer via v_layer (mirrors the JCE_RENDER_COOKIE_2D_ARRAY
// pattern). essl1/GLES2 is excluded from this program (no sampler2DArray), so
// LOW/WebGL1 falls back to the non-array PBR path.
SAMPLER2DARRAY(s_albedo, 0);
#else
SAMPLER2D(s_albedo,     0);
#endif
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
/* xy = UV tiling, zw = UV offset, for every map this material samples.
// The CPU substitutes 1 for a tiling of exactly 0 (jce_pbr_material.c): a
// zero would collapse each texture to one texel, which reads as "the
// textures are gone" rather than as a setting. */
uniform vec4 u_uvTransform;

uniform vec4 u_csmParams;
// Per-cascade world-units-per-shadow-texel scale, used to turn a normalized
// shadow-depth difference into a penumbra width.  Written by jce_sr_shadow.c
// from JceCsmData.depth_range; see pcss.sh.
uniform vec4 u_csmPenumbra;

// Per-cascade bias scale (x..w for cascades 0..3)
uniform vec4 u_csmBiasScales;
// Dual shadow maps (JCE_SHADOW_DUAL): dynamic-caster atlas params.
// x = tiles per side (2), y = 1 / dynAtlasSize, z = enabled (0/1), w = pad.
// Atlas is a 2x2 tile grid of the 4 cascades, bound to s_shadowMap (stage 5,
// dead under CSM); sample_csm_shadow min()s it into the static factor.
uniform vec4 u_csmDynParams;

// IBL samplers (stages 6-8)
SAMPLERCUBE(s_irradiance, 6);
SAMPLERCUBE(s_prefilter,  7);
SAMPLER2D(s_brdfLUT,      8);

// u_iblParams.x = IBL enabled (0 or 1), y = max prefilter mip level
// u_iblParams.z = OUTPUT GAMMA EXPONENT for the LDR path: 1/2.2 in LINEAR
//                 colour space, 1.0 in GAMMA (where the pow is a no-op and
//                 nothing decoded on the way in either).  Packed by the
//                 renderer from jce_texture_colour_space(), which is also what
//                 decides the hardware sRGB view, so the decode and the encode
//                 are one decision and cannot disagree.
// u_iblParams.w = linear output flag (1=skip gamma, for tonemap pass)
uniform vec4 u_iblParams;
/* EXTENDED LOBES -- glTF KHR_materials_clearcoat / _sheen.
 *   u_pbrLobes.x = clearcoat strength   (0 = the lobe is not evaluated)
 *   u_pbrLobes.y = clearcoat roughness
 *   u_pbrLobes.z = sheen roughness
 *   u_sheenColor.xyz = sheen colour, linear (0,0,0 = not evaluated)
 * Both predicates are ZERO-tested rather than a #ifdef: one program shades
 * every material, and a variant per lobe combination is four programs to
 * compile, ship and keep in step. */
uniform vec4 u_pbrLobes;
uniform vec4 u_sheenColor;
/* THE OTHER TWO LOBES.
 *   u_pbrLobes2.x = anisotropy strength   (0 = the lobe is not evaluated)
 *   u_pbrLobes2.y = anisotropy rotation, in TURNS about the surface normal
 *   u_pbrLobes2.z = translucency strength (0 = not evaluated)
 *   u_pbrLobes2.w = translucency thickness
 *   u_translucencyColor.xyz = the tint light picks up passing through
 *
 * Two more vec4 rather than filling the .w that u_pbrLobes and u_sheenColor
 * each leave free: those two frees are not adjacent and are not related to
 * these values, and a packing whose only justification is that a slot was
 * empty is the kind that gets read wrong later. */
uniform vec4 u_pbrLobes2;
uniform vec4 u_translucencyColor;
// Reflection-probe box for parallax correction, read only when
// u_giParams.w > 0.5: [0].xyz = centre (world), [1].xyz = half-extents.
uniform vec4 u_giProbeBox[2];

// ── Baked GI (P1-baked-gi-consume) ──────────────────────────────────
// SH9 ambient irradiance: 9 RGB coefficients (xyz used, w padding) from
// the nearest baked LightProbeGroup. Reconstructed per-fragment and added
// to the diffuse ambient when u_giParams.x > 0.5.
uniform vec4 u_sh9[9];
// u_giParams.x = SH9 ambient enabled (0/1) — REPLACES the irradiance source
//                (baked LightProbeGroup semantics: the bake IS the ambient)
// u_giParams.y = reflection-probe intensity (scales probe specular/diffuse)
// u_giParams.z = dynamic-GI additive SH9 (GI L1): u_sh9 holds a runtime
//                single-bounce estimate that ADDS to the normal ambient
//                instead of replacing it.  0 (every legacy path) = inert.
uniform vec4 u_giParams;

// Screen-space AO (SSAO).  x = enabled (>0.5), yz = 1/screenWidth, 1/screenHeight
// (so gl_FragCoord.xy * yz = screen UV).  Default 0 => off => material-AO path.
uniform vec4 u_ssaoParams;

// Reconstruct irradiance E(n) from the 9 SH coefficients. The CPU baker
// (jce_lightmapper_bake_sh9) already folds the cosine-lobe / PI weighting
// into the projection, so this is a direct dot with the SH basis at n.
vec3 sh9_irradiance(vec3 n)
{
    float x = n.x, y = n.y, z = n.z;
    float Y0 =  0.282095;
    float Y1 =  0.488603 * y;
    float Y2 =  0.488603 * z;
    float Y3 =  0.488603 * x;
    float Y4 =  1.092548 * x * y;
    float Y5 =  1.092548 * y * z;
    float Y6 =  0.315392 * (3.0 * z * z - 1.0);
    float Y7 =  1.092548 * x * z;
    float Y8 =  0.546274 * (x * x - y * y);

    vec3 e = u_sh9[0].xyz * Y0
           + u_sh9[1].xyz * Y1
           + u_sh9[2].xyz * Y2
           + u_sh9[3].xyz * Y3
           + u_sh9[4].xyz * Y4
           + u_sh9[5].xyz * Y5
           + u_sh9[6].xyz * Y6
           + u_sh9[7].xyz * Y7
           + u_sh9[8].xyz * Y8;
    return max(e, vec3_splat(0.0));
}

// CSM cascade samplers (stages 9-12)
SAMPLER2D(s_csmShadow0, 9);
SAMPLER2D(s_csmShadow1, 10);
SAMPLER2D(s_csmShadow2, 11);
SAMPLER2D(s_csmShadow3, 12);

// P3-E.5  — Light cookies + IES profiles (stages 13-14)
// P3-E.5b — Directional light cookie projection (shares sampler 13).
// Atlas slot index per-light is uploaded in u_dirLights[i*2+1].w and
// u_spotLights[i*4+3].y; the CPU side currently single-binds the
// first eligible cookie texture, but once s_cookie is promoted to
// SAMPLER2DARRAY the per-light slot can drive texture2DArray sampling
// with no further engine changes.  See engine/src/renderer/AGENTS.md.
/* ALWAYS the array now.  Each light's atlas slot rides in
 * u_dirLights[i*2+1].w / u_spotLights[i*4+3].y, so every light samples its
 * OWN cookie instead of borrowing the one image a single 2D bind could carry.
 *
 * No permutation guards this, and that is checked rather than assumed: bgfx
 * rewrites the shader's #version to 140 on this engine's GL floor (3.1, the
 * lowest of the three graphics tiers in conan/hooks/hook_bgfx_wasm_fix.py)
 * and then emits `#define texture2DArray texture` -- so profile-120 source
 * compiles to valid GLSL 1.40.  See renderer_gl.cpp's OPENGL >= 31 branch. */
SAMPLER2DARRAY(s_cookie, 13);

// ── Sampler stage 14: IES LUT (default) vs Forward+ cluster table ───────
// The cookie (stage 13) stays in both programs.  Stage 14 is multiplexed:
//   * DEFAULT (no JCE_FORWARDPLUS): s_iesLut — the per-spot IES photometric
//     profile, exactly as today.
//   * Forward+ variant: s_cluster — the SINGLE combined RGBA32F cluster data
//     texture (grid ++ index ++ lights row-regions) that jce_forwardplus
//     uploads.  IES is dropped in this variant (documented MVP tradeoff) so
//     no 17th sampler is needed; WebGL2/GLES3 stay at ≤16 fragment samplers.
#ifdef JCE_FORWARDPLUS
SAMPLER2D(s_cluster, 14);
// Cluster uniforms (see engine/include/jce/renderer/jce_forwardplus.h).
//   u_clusterParams  : x=CX, y=CY, z=CZ, w=lightCount
//   u_clusterParams2 : x=max_per_cell, y=texWidth(256), z=zNear, w=zFar
//   u_clusterRegions : x=gridBaseRow, y=indexBaseRow, z=lightsBaseRow,
//                      w=totalRows (texture height)
uniform vec4 u_clusterParams;
uniform vec4 u_clusterParams2;
uniform vec4 u_clusterRegions;
#else
SAMPLER2D(s_iesLut, 14);
#endif

// P1 — Local (spot/point) shadow atlas (sampler stage 15). Each shadow-casting
// spot renders a perspective depth tile into one atlas slot; u_spotShadowSlot[i]
// gives spot i's slot (-1 = no shadow). Stage 15 is shared with terrain layer2,
// but terrain uses fs_terrain.sc, so here 15 is unambiguously the shadow atlas.
SAMPLER2D(s_localShadowMap, 15);
uniform mat4 u_localShadowVP[4];   // per-slot light-view-proj
uniform vec4 u_spotShadowSlot;     // lane i = atlas slot for spot i (-1 = none)
uniform vec4 u_pointShadowSlot[4]; // 16 point lanes -> atlas slot (-1 = none)
// u_localShadowParams: x = tiles per atlas side, y = 1/atlasSize,
//                      z = depth bias (legacy global default), w = texel size
uniform vec4 u_localShadowParams;
// Per-slot depth bias: lane i = atlas slot i's authored shadowBias (4 slots).
uniform vec4 u_localShadowBias;
// #7 omnidirectional point cube shadows: 6 face VPs per budgeted point light
// (12 = JCE_POINT_SHADOW_MAX*6) + per-point face-0 atlas slot (16 lanes -> 4 vec4).
// base < 0 => this point has no cube shadow (shader takes the legacy single-tile
// u_pointShadowSlot path). Fixed layout: slots 0-3 spots/legacy, 4-9 cube point 0,
// 10-15 cube point 1, so vp index = (base-4) + face.
uniform mat4 u_pointCubeVP[12];
uniform vec4 u_pointCubeBaseSlot[4];

// Shadow FILTER quality tier (frame-constant; set by the scene renderer from
// the render-pipeline asset's shadow_filter_quality knob):
//   x < 0.5  ->  1 hard tap (local + CSM; cascade blend forced off CPU-side)
//   x < 1.5  ->  3x3 PCF everywhere
//   else     ->  full quality (local 3x3, CSM rotated 5x5 + cascade blend)
// y = SUN ANGULAR DIAMETER in DEGREES -- the apparent size of the sun, which
//     is what decides how fast a shadow's edge softens with distance from
//     whatever cast it.  Same quantity and unit as Godot's
//     DirectionalLight3D.angular_distance (the real sun is 0.53).  0 disables
//     contact hardening and the sun shadow is byte-identical to a build
//     without it.
// z = the cap on the widened kernel radius, in texels.  Not decoration: the
//     kernel is 25 taps whatever the radius, so an uncapped penumbra spreads
//     those taps until the shadow breaks into moving speckle -- which looks
//     worse than the hard edge it replaced.
// Frame-constant uniform branch: coherent for every fragment of every draw,
// so the untaken side's texture fetches are genuinely skipped on SM3+.
uniform vec4 u_shadowQuality;

// Core atlas-tile shadow test: project worldPos through `vp`, sample atlas tile
// `slot` (in the active tiles x tiles grid) with normal-offset bias + 3x3 PCF.
// Returns 1.0 (lit) when out of frustum / nearest occluder; 0.0 when occluded.
// Shared by spot/legacy-point (sampleLocalShadow) and omni cube (samplePointCubeShadow).
float sampleAtlasTile(int slotIndex, mat4 vp, vec3 worldPos, vec3 N,
                      vec3 lightPos, float ndotl, float bias)
{
    if (slotIndex < 0) return 1.0;

    float tiles  = max(u_localShadowParams.x, 1.0);

    // NORMAL-OFFSET bias — the real fix for the concentric "acne ring" moire a
    // PERSPECTIVE local-shadow map produces on a flat receiver. Push the sampled
    // position off the surface along the geometric normal by a few shadow texels'
    // WORLD size. Under perspective the texel world size grows with distance to
    // the light, so scale by that distance; widen further at grazing angles.
    float distToLight = length(lightPos - worldPos);
    float texelWorld  = distToLight * u_localShadowParams.y * tiles * 2.0;
    float nOff = texelWorld * (1.5 + (1.0 - clamp(ndotl, 0.0, 1.0)) * 3.0);
    vec3 sp = worldPos + N * nOff;

    vec4 clip = mul(vp, vec4(sp, 1.0));
    if (clip.w <= 0.0) return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    vec2 uv  = ndc.xy * 0.5 + vec2_splat(0.5);
    // V-flip convention MUST match the CSM path (sample_csm_shadow): flip on
    // D3D/Vulkan/Metal, NOT on GL. This was inverted, so on D3D the spot/point
    // shadow sampled a vertically MIRRORED map — the point light's straight-down
    // view has up=+Z, so its shadow appeared mirrored along world Z, and the
    // spot's false occlusion cut its lit pool in half.
#if !BGFX_SHADER_LANGUAGE_GLSL
    uv.y = 1.0 - uv.y;
#endif
#if BGFX_SHADER_LANGUAGE_GLSL
    float curDepth = ndc.z * 0.5 + 0.5;
#else
    float curDepth = ndc.z;
#endif
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;

    float tileUV = 1.0 / tiles;
    float col = mod(float(slotIndex), tiles);
    float row = floor(float(slotIndex) / tiles);
#if BGFX_SHADER_LANGUAGE_GLSL
    // GL FBO textures are bottom-left-origin: the tile bgfx places at view-rect
    // row r physically sits at texture row (tiles-1-r). Flip the tile ROW here;
    // the within-tile V correctly stays unflipped on GL (see uv.y above). On GL
    // without this, every lookup hit the WRONG atlas tile: cleared-to-far areas
    // (shadows had no effect) or a neighbour's depths (point pool falsely
    // occluded on one side of world Z).
    row = tiles - 1.0 - row;
#endif
    vec2 tileOrigin = vec2(col, row) * tileUV;

    // Small constant depth bias on top of the normal offset (caller supplies the
    // authored shadowBias; cube faces pass a fixed default).
    float b = max(bias, 0.0005);

    // 3x3 PCF, clamped inside this light's atlas tile so taps never bleed into
    // a neighbouring light's tile (soft edges).
    float texel = u_localShadowParams.y;       // 1 / atlasSize
    float lo = texel * 0.5;
    float hi = tileUV - texel * 0.5;
    vec2  inTile = uv * tileUV;

    // Tier 0: single hard tap. The normal-offset bias above is kept on every
    // tier (ALU-only, it is the acne fix); only the 9-tap PCF is skipped.
    if (u_shadowQuality.x < 0.5) {
        vec2 t0 = clamp(inTile, vec2_splat(lo), vec2_splat(hi));
        float d0 = texture2D(s_localShadowMap, tileOrigin + t0).r;
        return ((curDepth - b) <= d0) ? 1.0 : 0.0;
    }

    float sum = 0.0;
    for (int oy = -1; oy <= 1; oy++) {
        for (int ox = -1; ox <= 1; ox++) {
            vec2 t = clamp(inTile + vec2(float(ox), float(oy)) * texel,
                           vec2_splat(lo), vec2_splat(hi));
            float d = texture2D(s_localShadowMap, tileOrigin + t).r;
            sum += ((curDepth - b) <= d) ? 1.0 : 0.0;
        }
    }
    return sum * (1.0 / 9.0);
}

// Spot/legacy-point single-tile shadow: slot 0..3, VP in u_localShadowVP, with
// the per-slot authored bias. (Same signature as before — callers unchanged.)
float sampleLocalShadow(int slotIndex, vec3 worldPos, vec3 N,
                        vec3 lightPos, float ndotl)
{
    if (slotIndex < 0) return 1.0;
    float perSlotBias = (slotIndex == 0) ? u_localShadowBias.x :
                        (slotIndex == 1) ? u_localShadowBias.y :
                        (slotIndex == 2) ? u_localShadowBias.z : u_localShadowBias.w;
    return sampleAtlasTile(slotIndex, u_localShadowVP[slotIndex],
                           worldPos, N, lightPos, ndotl, perSlotBias);
}

// #7 omnidirectional point cube shadow. Pick the cube face by the major axis of
// (worldPos - lightPos); the face ORDER MUST MATCH jce_sr_shadow.c CUBE_DIRS:
// {+X,-X,+Y,-Y,+Z,-Z}. Tile = base+face; VP = u_pointCubeVP[(base-4)+face] (base
// is 4 or 10 by the fixed reserved layout, so the index is 0..11).
float samplePointCubeShadow(int baseSlot, vec3 worldPos, vec3 N,
                            vec3 lightPos, float ndotl)
{
    if (baseSlot < 0) return 1.0;
    vec3 dir = worldPos - lightPos;
    vec3 ad  = abs(dir);
    int face;
    if (ad.x >= ad.y && ad.x >= ad.z) face = (dir.x >= 0.0) ? 0 : 1;
    else if (ad.y >= ad.z)            face = (dir.y >= 0.0) ? 2 : 3;
    else                              face = (dir.z >= 0.0) ? 4 : 5;
    int slot  = baseSlot + face;
    int vpIdx = (baseSlot - 4) + face;   // base 4/10 -> 0..11 (dynamic, ES3-ok)
    return sampleAtlasTile(slot, u_pointCubeVP[vpIdx],
                           worldPos, N, lightPos, ndotl, 0.0015);
}

// IES ONLY.  The cookie components this vec4 used to carry -- has_cookie,
// strength, and which spot light owned the single sampler bind -- are now in
// each light's OWN pack (u_spotLights[i*4+3].y = atlas layer, .z = strength),
// because "which one light gets the cookie" was the whole bug.
//
// u_iesParams.x = has_ies_spot   (1.0 / 0.0)
// u_iesParams.y = ies_spot_index (which u_spotLights[] slot owns the one
//                                  photometric LUT sampler 14 can carry)
// u_iesParams.zw = 0, unread; a vec4 is the smallest uniform this API has.
uniform vec4 u_iesParams;

// u_cookieSpotVP is gone: one shared clip-from-world matrix could only be
// right for one light, and fs_pbr_main.sh now derives each spot light's
// cookie frustum from its own position, direction and cone.

// P3-E.5b — Directional cookie.
// u_cookieDirParams.x = has_cookie_dir   (1.0 / 0.0)
// u_cookieDirParams.y = cookie_strength  (0..1)
// u_cookieDirParams.z = cookie_dir_index (which u_dirLights[] slot owns
//                                          the cookie sampler binding)
// u_cookieDirParams.w = 0
uniform vec4 u_cookieDirParams;
uniform mat4 u_cookieDirVP;

// ── Streaming LOD cross-fade (Direction B; dithered detail fade-in) ─────
// u_lodFade.x = fade factor in [0,1] (0 = fully dithered out / invisible,
//               1 = fully present); .w = active flag (>0.5 enables the
//               screen-door discard).  DEFAULT is {1,0,0,0} (active=0), so
//               the discard block below is skipped entirely and every
//               normal draw is byte-identical to the pre-feature shader.
//               Only the static-mesh draw path that is actively fading a
//               freshly-streamed entity sets active=1.
uniform vec4 u_lodFade;

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

// Needs csm_sample_depth (above) and u_shadowQuality / u_csmPenumbra.
#include "pcss.sh"
#include "shadow_bias.sh"

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
    // Larger normal-offset floor (0.35 vs 0.15) plus a multiplier so that
    // surfaces nearly facing the light still get pushed off the texel grid
    // by ~2-3 world texels.  This is the primary defence against the
    // "wood-grain" parallel-stripe acne reported during camera rotation.
    float normal_offset = u_csmParams.z * bias_scale * 2.5 * max(sin_theta, 0.35);
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
        // Sentinel: this cascade's light-space square does NOT cover the
        // fragment.  The caller falls through to a wider cascade; only if no
        // cascade covers it (beyond the shadow range) is it treated as lit.
        // Returning 1.0 here was the bug behind the triangular bright wedges.
        return -1.0;
    }

    float inv_map_size = max(u_csmParams.x, 1.0 / 2048.0);
    vec2 texel = vec2_splat(inv_map_size);
    float cascade_lerp = clamp(float(cascade) * (1.0 / 3.0), 0.0, 1.0);

    // Depth bias in SHADOW TEXELS of world offset -- see shadow_bias.sh.
    // The mesh figure keeps its historical 3x over the terrain's (4.8 vs 1.6
    // texels) and its steeper slope term; what goes is the mix(3,6,cascade)
    // ladder and the mix(0.01,0.08,cascade) ceiling, both of which existed to
    // compensate, one constant at a time, for a bias whose world meaning was
    // the cascade fit's geometry.  The comment they carried said so: "the
    // much larger world depth range it represents".  That range is now
    // divided out rather than budgeted around.
    float slope = sin_theta / max(ndotl, 0.1);
    float depth_bias = jce_shadow_depth_bias(cascade, 4.8, slope, 8.0);

    float filter_radius = max(u_csmParams.w, 0.5) * mix(1.0, 2.0, cascade_lerp);

    // Contact hardening.  Only on the full tier: the widened kernel is
    // what spends the extra taps, and at 1 or 9 taps a wide kernel is
    // speckle rather than softness.  The gate is a frame-constant uniform
    // branch, so tiers 0 and 1 pay nothing for this.
    if (u_shadowQuality.x >= 1.5)
    {
        filter_radius = pcss_filter_radius(cascade, csm_uv, csm_z,
                                           depth_bias, texel, filter_radius);
    }

    // Shadow filter tier (see u_shadowQuality). GLSL-120 safety rule: a
    // uniform branch selecting between CONSTANT-bound loops — never a
    // variable loop bound, never `continue`.
    // Tier 0: single hard tap; the bias math above stays (it is ALU-only
    // and remains the acne defence), and the hash-rotation sin/cos is
    // skipped along with the 25-tap kernel.
    // STATIC shadow factor `s` (this cascade's csm_tex).  Computed per filter
    // tier, then — for dual shadow maps — min()'d with the dynamic atlas below
    // through a SINGLE return so cascade fallthrough/blend/edge logic inherits it.
    float s;
    if (u_shadowQuality.x < 0.5)
    {
        // Tier 0: single hard tap.
        float depth0 = csm_sample_depth(cascade, csm_uv);
        s = (csm_z - depth_bias > depth0) ? 0.0 : 1.0;
    }
    else if (u_shadowQuality.x < 1.5)
    {
        // Tier 1: unrotated 3x3 PCF (9 taps).
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
        s = sum9 / 9.0;
    }
    else
    {
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
        s = sum / 25.0;
    }

    // Dual shadow maps: min() with the DYNAMIC-caster atlas tile for this cascade
    // (movers, rendered separately with the SAME cascade VP into s_shadowMap as a
    // 2x2 tile grid).  Occluded if EITHER the static map OR the dynamic map
    // occludes — a 3x3 PCF clamped inside the tile so it can't bleed into a
    // neighbour cascade.  Enabled lane is 0 unless dual mode is active this frame,
    // so the whole block is a no-op (byte-identical) otherwise.
    if (u_csmDynParams.z > 0.5)
    {
        float tiles = u_csmDynParams.x;            // 2.0
        float invT  = 1.0 / tiles;
        float col   = mod(float(cascade), tiles);
        float row   = floor(float(cascade) / tiles);
#if BGFX_SHADER_LANGUAGE_GLSL
        row = tiles - 1.0 - row;                   // GL bottom-left tile-row flip
#endif
        vec2 org = vec2(col, row) * invT;
        vec2 dt  = vec2_splat(u_csmDynParams.y);
        vec2 lo  = org + dt * 0.5;
        vec2 hi  = org + invT - dt * 0.5;
        vec2 ctr = clamp(org + csm_uv * invT, lo, hi);
        // Match the dynamic tap count to the STATIC filter tier so the min-combine
        // — which runs for EVERY shadowed pixel, movers or not — costs the same as
        // the static sample it augments.  Tier 0 (iGPU) = 1 hard tap keeps the
        // dynamic add ~free; higher tiers use 3x3 PCF for a soft mover edge.
        if (u_shadowQuality.x < 0.5)
        {
            float dd = texture2D(s_shadowMap, ctr).r;
            s = min(s, (csm_z - depth_bias > dd) ? 0.0 : 1.0);
        }
        else
        {
            float dsum = 0.0;
            for (int dy = -1; dy <= 1; dy++)
            {
                for (int dx = -1; dx <= 1; dx++)
                {
                    vec2 t = clamp(org + csm_uv * invT + vec2(float(dx), float(dy)) * dt,
                                   lo, hi);
                    float dd = texture2D(s_shadowMap, t).r;
                    dsum += (csm_z - depth_bias > dd) ? 0.0 : 1.0;
                }
            }
            s = min(s, dsum * (1.0 / 9.0));
        }
    }
    return s;
}

vec3 safe_normalize_vec3(vec3 value, vec3 fallback)
{
    float len2 = dot(value, value);
    if (len2 > 1e-8)
        return value * inversesqrt(len2);
    return fallback;
}

#ifdef JCE_FORWARDPLUS
// ── Forward+ cluster helpers ────────────────────────────────────────────
// All three regions live in ONE RGBA32F texture (s_cluster) of fixed width
// W = u_clusterParams2.y (256), stacked as GRID / INDEX / LIGHTS row-regions.
// Within a region, element e maps to column (e % W); its ABSOLUTE row is
// regionBaseRow + (e / W).  We fetch with NEAREST point-clamp sampling at the
// texel CENTER, which is exact for an integer element index (no texelFetch so
// the same code path works on GLSL 120 / WebGL2 without integer fetch).

// Fetch the RGBA32F texel for in-region element `e` at absolute base row
// `baseRow`.  texWidth = u_clusterParams2.y; totalRows = u_clusterRegions.w.
vec4 fp_fetch(float e, float baseRow)
{
    float W = u_clusterParams2.y;
    float col = mod(e, W);                 // e % W   (exact for integer e)
    float rowInRegion = floor(e / W);      // e / W
    float texelRow = baseRow + rowInRegion;
    vec2 uv = vec2((col + 0.5) / W,
                   (texelRow + 0.5) / u_clusterRegions.w);
    // EXPLICIT LOD 0: the cluster data texture has no mips, so LOD 0 is exact.
    // Crucially this avoids an implicit-gradient texture op inside the varying
    // per-froxel loop — on HLSL/dx11 a gradient instruction in a varying loop
    // forces the compiler to UNROLL it, and unrolling JCE_FP_MAX_PER_CELL
    // texture-sampling iterations overflows (error X3511). texture2DLod keeps
    // the loop dynamic and compiles cross-backend (dx11/spv/glsl/essl3).
    return texture2DLod(s_cluster, uv, 0.0);
}

// Does a cluster light whose mask is (lo, hi) reach a receiver on `layer`?
//
// WHY THE MASK TRAVELS WITH THE LIGHT.  The cluster is built ONCE PER FRAME
// with no receiver in sight, so the CPU cannot mask these the way it masks the
// brute-force arrays -- it does not yet know who is receiving.  Sending the
// mask along and testing it here is what lets Forward+ honour rendering layers
// at all; before this the whole clustered path was turned off for any frame
// carrying a mask, which is a performance cliff on exactly the hardware
// clustered lighting exists for.
//
// WHY TWO HALVES.  A 32-bit mask does not survive a float: 2^31 needs a 32-bit
// mantissa and there are 24.  Each half is an integer below 65536 -- exact --
// and so is floor(m / 2^b) for b < 16, which is all the test needs.
//
// WHY mod/floor/exp2 AND NOT `&`.  The OpenGL backend compiles at GLSL 1.20,
// which has NO integer bitwise operators at all.  This is the form that
// survives the floor.
//
// mask 0 means EVERY layer, matching JceDirLightDesc.layer_mask's contract --
// so a scene that never touches layers takes the first branch and pays a
// compare.
bool fp_light_reaches(float maskLo, float maskHi, float layer)
{
    if (maskLo == 0.0 && maskHi == 0.0) return true;
    float m = (layer < 16.0) ? maskLo : maskHi;
    float b = (layer < 16.0) ? layer  : (layer - 16.0);
    return mod(floor(m / exp2(b)), 2.0) >= 0.5;
}

// Mirror jce_light_cluster_slice_for_view_z EXACTLY (log-Z).
//   vz <= zNear           -> 0
//   vz >= zFar            -> CZ-1
//   else floor( log(vz/zNear)/log(zFar/zNear) * CZ ), clamped.
float fp_slice_for_view_z(float vz)
{
    float CZ    = u_clusterParams.z;
    float zNear = u_clusterParams2.z;
    float zFar  = u_clusterParams2.w;
    if (vz <= zNear) return 0.0;
    if (vz >= zFar)  return CZ - 1.0;
    float t = log(vz / zNear) / log(zFar / zNear);
    float s = floor(t * CZ);
    return clamp(s, 0.0, CZ - 1.0);
}
#endif // JCE_FORWARDPLUS

/* Missing-texture checker lives in pbr_common.sh (missing_texture_checker):
   UV-space pattern with a dominant-axis local-space fallback. */

// ── Streaming LOD cross-fade dither test (Direction B) ──────────────────
// Returns true when the current fragment should be DISCARDED for the
// dithered fade-in.  The 4x4 Bayer ordered-dither threshold is computed
// arithmetically (NO GLSL array literals like `float t[16]=float[](...)`,
// which break some bgfx shaderc backends).  The full block is gated so it
// is a strict no-op unless u_lodFade is explicitly activated AND the fade
// is still in progress (< ~1.0):
//   * u_lodFade.w <= 0.5  -> inactive (default {1,0,0,0}) -> never discard
//   * u_lodFade.x >= 0.999 -> fully faded in              -> never discard
// fade.x = 0 discards every fragment; fade.x ramping 0->1 dissolves it in.
// fragCoord MUST be passed in (gl_FragCoord.xy from the caller in main()):
// bgfx/shaderc only exposes gl_FragCoord inside main() — referencing it in a
// free function fails to compile on the HLSL (dx11) backend ("unknown variable
// gl_FragCoord").  Taking it as a parameter keeps this helper portable.
bool jce_lod_fade_discard(vec2 fragCoord)
{
    if (u_lodFade.w <= 0.5)   return false; // inactive (default)
    if (u_lodFade.x >= 0.999) return false; // fully present -> keep all

    // 4x4 Bayer matrix, value in [0,1), built without array literals.
    // Standard recursive Bayer(4): index = 16*B4[y][x] mapped to threshold
    // (B + 0.5)/16 to center the levels.  We reconstruct B4 arithmetically.
    int bx = int(mod(fragCoord.x, 4.0));
    int by = int(mod(fragCoord.y, 4.0));

    // Bayer-4 matrix values (0..15):
    //   row0:  0  8  2 10
    //   row1: 12  4 14  6
    //   row2:  3 11  1  9
    //   row3: 15  7 13  5
    float b = 0.0;
    if (by == 0) {
        b = (bx == 0) ? 0.0  : (bx == 1) ?  8.0 : (bx == 2) ?  2.0 : 10.0;
    } else if (by == 1) {
        b = (bx == 0) ? 12.0 : (bx == 1) ?  4.0 : (bx == 2) ? 14.0 :  6.0;
    } else if (by == 2) {
        b = (bx == 0) ? 3.0  : (bx == 1) ? 11.0 : (bx == 2) ?  1.0 :  9.0;
    } else {
        b = (bx == 0) ? 15.0 : (bx == 1) ?  7.0 : (bx == 2) ? 13.0 :  5.0;
    }
    float bayer = (b + 0.5) * (1.0 / 16.0); // (0.5..15.5)/16 -> (0,1)
    return (u_lodFade.x < bayer);
}
