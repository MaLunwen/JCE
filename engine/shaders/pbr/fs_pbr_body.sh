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
uniform vec4 u_csmParams;
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
// u_iblParams.w = linear output flag (1=skip gamma, for tonemap pass)
uniform vec4 u_iblParams;

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
#ifdef JCE_RENDER_COOKIE_2D_ARRAY
SAMPLER2DARRAY(s_cookie, 13);
#else
SAMPLER2D(s_cookie, 13);
#endif

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

// u_cookieParams.x = has_cookie_spot   (1.0 / 0.0)
// u_cookieParams.y = cookie_strength   (0..1)
// u_cookieParams.z = has_ies_spot      (1.0 / 0.0)
// u_cookieParams.w = cookie_spot_index (which u_spotLights[] slot owns
//                                        the cookie/IES sampler binding)
uniform vec4 u_cookieParams;

// Clip-from-world for the spot light that owns the cookie/IES binding.
uniform mat4 u_cookieSpotVP;

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

    // Constant + slope-scaled depth bias.  Bumped multipliers (3.0 base,
    // up to 6.0 on cascade 3, slope×8) to guarantee ndcZ - bias < depth
    // for grazing surfaces — the depth gradient across one shadow texel
    // is the primary cause of parallel-stripe shadow acne.
    float slope = sin_theta / max(ndotl, 0.1);
    float depth_bias = inv_map_size * mix(3.0, 6.0, cascade_lerp)
                     * bias_scale * (1.0 + slope * 8.0);
    // Cascade-aware ceiling: cascade 0 stays tight (≤0.01) while
    // cascade 3 may reach 0.08 without visible light bleeding given
    // the much larger world depth range it represents.
    depth_bias = min(depth_bias, mix(0.01, 0.08, cascade_lerp));

    float filter_radius = max(u_csmParams.w, 0.5) * mix(1.0, 2.0, cascade_lerp);

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

void main()
{
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
    // Base color texture is sRGB-encoded (glTF spec §5.19). Convert texture
    // to linear space FIRST, then multiply by the linear baseColorFactor.
#ifdef JCE_TEX_ARRAY
    // Texture-diverse instancing: the vertex shader packs this instance's albedo
    // 2D-array layer into v_tint.x (the array variant carries no colour tint).
    vec4 texColor = texture2DArray(s_albedo, vec3(v_texcoord0, v_tint.x));
#else
    vec4 texColor = texture2D(s_albedo, v_texcoord0);
#endif
    bool useCheckerFallback = (u_normalScale.x < 0.0);
    if (useCheckerFallback)
    {
        texColor = vec4(missing_texture_checker(v_localpos, v_texcoord0), 1.0);
    }

    vec3 albedo;
    if (useCheckerFallback)
    {
        albedo = clamp(texColor.rgb, vec3_splat(0.0), vec3_splat(1.0))
               * u_baseColorFactor.rgb;
    }
    else
    {
        albedo = pow(clamp(texColor.rgb, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2))
               * u_baseColorFactor.rgb;
    }
    float alpha = texColor.a * u_baseColorFactor.a;

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
        vec3 outRgb = pow(max(albedo, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));
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
        vec3 tangentNormal = texture2D(s_normalMap, v_texcoord0).xyz * 2.0 - vec3_splat(1.0);
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
    vec4 mrSample = texture2D(s_metalRough, v_texcoord0);
    float metallic  = mrSample.b * u_pbrParams.x;
    float roughness = mrSample.g * u_pbrParams.y;

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
        ao = texture2D(s_aoMap, v_texcoord0).r;
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
#ifdef JCE_RENDER_COOKIE_2D_ARRAY
                float _dirSlice = u_cookieDirParams.w; /* atlas layer for this dir light */
                vec4  cookieDSample = texture2DArray(s_cookie, vec3(uvD, _dirSlice));
#else
                vec4  cookieDSample = texture2D(s_cookie, uvD);
#endif
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
        // Only the spot light selected by u_cookieParams.w receives
        // the cookie / IES sampler bind (bgfx samplers are global per
        // draw, so v1 supports at most one cookie + one IES profile
        // per frame). Other lights pass through unchanged.
        if (u_cookieParams.x > 0.5 && float(i) == u_cookieParams.w)
        {
            vec4 clip = mul(u_cookieSpotVP, vec4(v_worldpos, 1.0));
            // Reject points behind the light's near plane.
            if (clip.w > 0.0)
            {
                vec3 ndc = clip.xyz / clip.w;
                vec2 uv  = ndc.xy * 0.5 + vec2(0.5, 0.5);
            #if BGFX_SHADER_LANGUAGE_GLSL
                // bgfx framebuffer y is flipped on GL; cookie textures
                // are authored top-left origin, mirror to match D3D.
                uv.y = 1.0 - uv.y;
            #endif
                // Clip outside the frustum.
                vec2 inUV = step(vec2_splat(0.0), uv) * step(uv, vec2_splat(1.0));
                float inMask = inUV.x * inUV.y;
#ifdef JCE_RENDER_COOKIE_2D_ARRAY
                float _spotSlice = u_cookieParams.x; /* atlas layer for this spot light */
                vec4  cookieSample = texture2DArray(s_cookie, vec3(uv, _spotSlice));
#else
                vec4  cookieSample = texture2D(s_cookie, uv);
#endif
                // Blend toward white by (1 - strength) so a partial
                // strength still tints rather than fully masks.
                vec3 cookieRGB = mix(vec3_splat(1.0),
                                     cookieSample.rgb,
                                     u_cookieParams.y * inMask);
                radiance *= cookieRGB;
            }
        }

        // ── P3-E.5: IES photometric profile ────────────────────────
        // Sample the 1-D LUT (256x1) by the angle between the light's
        // forward and the surface->light vector.
        if (u_cookieParams.z > 0.5 && float(i) == u_cookieParams.w)
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

    // --- Emissive ---
    // Emissive texture is also sRGB-encoded; convert to linear first,
    // then multiply by the linear emissive factor.
    vec3 emissiveTex = pow(clamp(texture2D(s_emissive, v_texcoord0).rgb, vec3_splat(0.0), vec3_splat(1.0)), vec3_splat(2.2));
    vec3 emissive = emissiveTex * u_emissiveFactor.xyz;

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
        color = pow(max(color, vec3_splat(0.0)), vec3_splat(1.0 / 2.2));

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
