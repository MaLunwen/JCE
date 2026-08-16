/*
 * csm_shadow.sh -- cascaded shadow-map sampling, shared by every lit surface.
 *
 * This was duplicated between fs_terrain.sc and the water surface, which is how
 * shadow behaviour drifts: a bias tweak or a cascade-fallthrough fix lands in
 * one copy and the other keeps the old bug, and nothing about the result says
 * "these disagree" -- one surface just shadows slightly differently than the
 * one next to it.
 *
 * `csm_shadow_factor()` is the whole decision: cascade selection by view depth,
 * fallthrough to wider cascades when a fragment lands outside its own cascade's
 * light-space square, the in-cascade blend band, and the distance fade.  Callers
 * supply geometry and get a multiplier; they do not get to reimplement the
 * policy.
 *
 * fs_pbr_body.sh keeps its own variant deliberately -- it also handles the dual
 * static/dynamic atlas, which nothing here binds.  Merging that one is a
 * separate change with real regression surface on the main lit path.
 *
 * Requires, from the includer: u_csmVP[4], u_csmSplits, u_csmParams,
 * u_csmBiasScales, u_shadowQuality, and s_csmShadow0..3 on stages 9-12.
 */

#ifndef CSM_SHADOW_SH
#define CSM_SHADOW_SH

/* Stage 5 under CSM: the DYNAMIC-caster atlas (see sr_bind_frame_shadow_state).
 * The single-light shadow map is dead whenever cascades are active, which is
 * what frees the stage -- this engine has all sixteen spoken for otherwise.
 * Declared here rather than per-consumer because sample_csm_shadow below is
 * what reads it. */
SAMPLER2D(s_shadowMap, 5);

/* {tiles, 1/atlas_size, enabled, 0}. `enabled` is 0 on any frame without a
 * live dynamic atlas, which makes the whole block below a no-op and keeps a
 * scene with no movers byte-identical to the single-map path. */
uniform vec4 u_csmDynParams;

/* min() the dynamic-caster atlas tile for this cascade into a static-map
 * result.
 *
 * Occluded if EITHER map occludes -- the two hold disjoint caster sets, so a
 * min is the whole combine. Ported from fs_pbr_body.sh, which has sampled this
 * atlas since it was written while every surface reached through this header
 * ignored it. That asymmetry is not subtle in a scene: with the dual atlas on
 * by default, a rigidbody or a character casts a shadow onto a MESH floor and
 * none at all onto TERRAIN, water or grass. It reads as the shadow winking out
 * as the object crosses from one surface onto the other, which is a thing a
 * player does constantly and an investigator almost never does on purpose.
 *
 * The tile grid, the GL row flip and the clamped 3x3 are the same arithmetic
 * as the PBR copy; the two must agree because they address one texture. */
float csm_dyn_min(float s, int cascade, vec2 csm_uv, float csm_z,
                  float depth_bias)
{
    if (u_csmDynParams.z <= 0.5) return s;

    float tiles = u_csmDynParams.x;
    float invT  = 1.0 / tiles;
    float col   = mod(float(cascade), tiles);
    float row   = floor(float(cascade) / tiles);
#if BGFX_SHADER_LANGUAGE_GLSL
    row = tiles - 1.0 - row;                   /* GL bottom-left tile-row flip */
#endif
    vec2 org = vec2(col, row) * invT;
    vec2 dt  = vec2_splat(u_csmDynParams.y);
    vec2 lo  = org + dt * 0.5;
    vec2 hi  = org + invT - dt * 0.5;

    /* Tap count follows the static filter tier, so the combine costs what the
     * sample it augments costs. */
    if (u_shadowQuality.x < 0.5)
    {
        vec2 ctr = clamp(org + csm_uv * invT, lo, hi);
        float dd = texture2D(s_shadowMap, ctr).r;
        return min(s, (csm_z - depth_bias > dd) ? 0.0 : 1.0);
    }

    float dsum = 0.0;
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            vec2 t = clamp(org + csm_uv * invT
                           + vec2(float(dx), float(dy)) * dt, lo, hi);
            float dd = texture2D(s_shadowMap, t).r;
            dsum += (csm_z - depth_bias > dd) ? 0.0 : 1.0;
        }
    }
    return min(s, dsum * (1.0 / 9.0));
}

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
    float normal_offset = u_csmParams.z * bias_scale * max(sin_theta, 0.15);
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
        // Sentinel "not covered" — caller falls through to a wider cascade.
        return -1.0;
    }

    float inv_map_size = max(u_csmParams.x, 1.0 / 2048.0);
    vec2 texel = vec2_splat(inv_map_size);
    float cascade_lerp = clamp(float(cascade) * (1.0 / 3.0), 0.0, 1.0);

    // Depth bias: constant component + slope-scaled component to handle
    // grazing-angle shadow acne (parallel-stripe wood-grain pattern).
    float slope = sin_theta / max(ndotl, 0.1);
    float depth_bias = inv_map_size * mix(1.0, 2.0, cascade_lerp)
                     * bias_scale * (1.0 + slope * 4.0);
    depth_bias = min(depth_bias, 0.01);

    float filter_radius = max(u_csmParams.w, 0.5) * mix(1.0, 2.0, cascade_lerp);

    // Shadow filter tier (see u_shadowQuality; mirrors fs_pbr.sc). GLSL-120
    // safety rule: uniform branch selecting between CONSTANT-bound loops —
    // never a variable loop bound, never `continue`.
    // Tier 0: single hard tap (bias math above stays; hash rotation skipped).
    if (u_shadowQuality.x < 0.5)
    {
        float depth0 = csm_sample_depth(cascade, csm_uv);
        float s0 = (csm_z - depth_bias > depth0) ? 0.0 : 1.0;
        return csm_dyn_min(s0, cascade, csm_uv, csm_z, depth_bias);
    }

    // Tier 1: unrotated 3x3 PCF (9 taps).
    if (u_shadowQuality.x < 1.5)
    {
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
        return csm_dyn_min(sum9 / 9.0, cascade, csm_uv, csm_z, depth_bias);
    }

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
    return csm_dyn_min(sum / 25.0, cascade, csm_uv, csm_z, depth_bias);
}

/*
 * The complete directional-shadow multiplier for a world-space fragment.
 * Returns 1.0 for fully lit.  `view_depth` is positive view-space distance.
 */
/* The debug view mode reports which cascade a fragment was shadowed BY.
 *
 * It gets that number out of this function rather than re-deriving it, and
 * that is the whole point.  A visualiser that mirrors the selection logic is
 * a visualiser that can disagree with the renderer -- silently, and exactly
 * when something is wrong, which is the only time anyone looks at it.  So
 * `out_cascade` is written by the same statements that choose the cascade
 * actually sampled, including the fallthrough to wider cascades, and
 * csm_shadow_factor() below is a thin wrapper that discards it.
 *
 * -1 means "past every cascade": the fragment is lit because nothing covers
 * it, which is a different thing from being lit because nothing occludes it,
 * and the two are indistinguishable in the returned factor alone. */
float csm_shadow_factor_dbg(vec3 world_pos, vec3 normal, vec3 to_light_dir,
                            float view_depth, out float out_cascade)
{
    out_cascade = -1.0;
    if (u_csmSplits.x <= 0.0) return 1.0;

    float fragDepth = max(view_depth, 0.0);

    int cascade = 3;
    if      (fragDepth < u_csmSplits.x) cascade = 0;
    else if (fragDepth < u_csmSplits.y) cascade = 1;
    else if (fragDepth < u_csmSplits.z) cascade = 2;

    int sel_cascade = cascade;
    float shadow = sample_csm_shadow(cascade, world_pos, normal, to_light_dir);

    // Cascade FALLTHROUGH: a depth-bucketed fragment can land outside its own
    // cascade's light-space square.  Stepping to wider cascades is what stops
    // it being wrongly left fully lit -- the symptom is a triangular bright
    // wedge, which reads as a lighting choice rather than a bug.
    if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, world_pos, normal, to_light_dir); }
    if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, world_pos, normal, to_light_dir); }
    if (shadow < 0.0 && cascade < 3) { cascade = cascade + 1; shadow = sample_csm_shadow(cascade, world_pos, normal, to_light_dir); }

    /* Cascade SQUARE-EDGE cross-fade.
     *
     * The depth-split blend below softens the boundary in DEPTH. This softens
     * the other one: the edge of the cascade's light-space SQUARE, which is
     * where the fallthrough ladder above hands a fragment to a wider cascade.
     * Each cascade's bias is balanced against its own texel size, and that
     * balance is discontinuous across the square's XY edge -- so without a
     * fade there is a straight knife-line step across every receiver the seam
     * crosses, and because the square is fitted to the camera it SWEEPS as the
     * camera moves.
     *
     * fs_pbr_body.sh has carried this since it was written, with a comment
     * naming the symptom in those words. csm_shadow.sh did not, and
     * csm_shadow.sh is what the TERRAIN and the WATER use -- the two surfaces
     * that are large enough and smooth enough for a straight line across them
     * to be unmistakable. A per-pixel map of frame-to-frame instability, with
     * the same sequence captured shadows-on and shadows-off and subtracted,
     * shows the seam as a hard-edged quadrilateral wedge lying across the bay.
     *
     * Same 8% band and same one extra tap as the mesh path, so the two agree
     * at a silhouette instead of stepping against each other. */
    if (shadow >= 0.0 && cascade < 3)
    {
        vec4  edgeClip = csm_clip_for_cascade(cascade, world_pos);
        vec2  edgeUv   = edgeClip.xy / edgeClip.w * 0.5 + 0.5;
        float sqEdge   = min(min(edgeUv.x, 1.0 - edgeUv.x),
                             min(edgeUv.y, 1.0 - edgeUv.y));
        if (sqEdge < 0.08)
        {
            float edge_shadow = sample_csm_shadow(cascade + 1, world_pos,
                                                  normal, to_light_dir);
            if (edge_shadow >= 0.0)
                shadow = mix(edge_shadow, shadow,
                             clamp(sqEdge / 0.08, 0.0, 1.0));
        }
    }

    if (shadow >= 0.0 && cascade == sel_cascade && cascade < 3)
    {
        float split_start = 0.0;
        float split_end   = u_csmSplits.x;
        if (cascade == 1)      { split_start = u_csmSplits.x; split_end = u_csmSplits.y; }
        else if (cascade == 2) { split_start = u_csmSplits.y; split_end = u_csmSplits.z; }

        float split_span = split_end - split_start;
        if (split_span > 0.001)
        {
            float blend_fraction = clamp(u_csmParams.y, 0.0, 0.35);
            float blend_range    = max(split_span * blend_fraction, 0.001);
            // Blend entirely WITHIN the current cascade: blending across the
            // boundary produces a 50%->100% discontinuity exactly where the
            // eye is already looking for a seam.
            float blend = smoothstep(split_end - blend_range, split_end, fragDepth);
            if (blend > 0.0001)
            {
                float next_shadow = sample_csm_shadow(cascade + 1, world_pos,
                                                      normal, to_light_dir);
                if (next_shadow >= 0.0) shadow = mix(shadow, next_shadow, blend);
            }
        }
    }

    // Beyond all cascades: lit.  Then a soft fade to the shadow far distance so
    // the outer edge is a gradient rather than a visible circle.
    out_cascade = (shadow < 0.0) ? -1.0 : float(cascade);
    if (shadow < 0.0) shadow = 1.0;
    float shadow_far = u_csmSplits.w;
    if (shadow_far > 0.0)
        shadow = mix(shadow, 1.0, smoothstep(shadow_far * 0.85, shadow_far, fragDepth));

    return shadow;
}

/* The production entry point: unchanged signature, unchanged behaviour. */
float csm_shadow_factor(vec3 world_pos, vec3 normal, vec3 to_light_dir,
                        float view_depth)
{
    float ignored;
    return csm_shadow_factor_dbg(world_pos, normal, to_light_dir,
                                 view_depth, ignored);
}

#endif // CSM_SHADOW_SH
