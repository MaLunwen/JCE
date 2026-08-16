#ifndef FOG_APPLY_SH
#define FOG_APPLY_SH

/*
 * fog_apply.sh — per-fragment aerial-perspective fog (Stage 1a.3).
 *
 * Pure ALU, ZERO new samplers / views / RTs (WebGL2 <=16 sampler budget
 * unchanged). Composites in LINEAR space: the caller MUST invoke this on the
 * lit linear `color` AFTER (ambient + Lo + emissive) and BEFORE the
 * `pow(1/2.2)` gamma encode. Gate `u_fogParams.x < 0.5` (NONE) => bit-exact
 * no-op so the default frame is byte-identical (render_parity baseline).
 *
 * Data carrier = the EXISTING JceSceneRenderingSettings fog_* fields (CANON):
 *   u_fogParams   = {mode(0..3), density, start, end}
 *   u_fogColor    = {horizon.rgb, strength}
 *   u_fogColorSun = {warm.rgb, height_falloff}
 * "Aerial perspective" == height-attenuated per-fragment fog with a warm
 * sun-side shift; mode maps to JceSceneFogMode (1=LINEAR,2=EXP,3=EXP2).
 *
 * Mirror of tests/middleware/scene/test_jce_aerial_fog_math.c — keep in sync.
 */

uniform vec4 u_fogParams;    // x=mode, y=density, z=start, w=end
uniform vec4 u_fogColor;     // xyz=horizon color, w=strength
uniform vec4 u_fogColorSun;  // xyz=warm sun-side color, w=height falloff
/* x = height origin (world Y datum for the height integral); yzw reserved.
 *
 * A fourth vec4 rather than a spare component of the three above, because
 * there is no spare: {mode,density,start,end} + {horizon.rgb,strength} +
 * {sunTint.rgb,falloff} is twelve floats and twelve are named. The one that
 * LOOKS free -- u_fogColor.w, "strength" -- has exactly one writer in the
 * engine and it is the literal 1.0f (jce_scene_renderer.c), so `f * strength`
 * is a multiply by one today. That makes it an empty slot but not a FREE one:
 * strength and a world-Y datum are different physical quantities, and taking
 * the slot would mean the next person to give strength a writer silently
 * moves the fog's height datum instead. */
uniform vec4 u_fogHeight;

vec3 apply_aerial_fog(vec3 litLinear, float viewDepth, vec3 worldPos,
                      vec3 cameraPos, vec3 sunDirToLight)
{
    // Gate: NONE (mode < 0.5) => algebraic no-op (baseline byte-identity).
    if (u_fogParams.x < 0.5)
        return litLinear;

    float mode    = u_fogParams.x;
    float density = u_fogParams.y;
    float start   = u_fogParams.z;
    float endd    = u_fogParams.w;

    // The distance the light actually travelled: RADIAL, from the eye to the
    // fragment. Computed here rather than taken from the caller, and that is
    // the whole point of this block.
    //
    // The parameter is named viewDepth and this comment used to say "the
    // positive view-space distance (v_viewdepth)". Three of the four callers
    // obeyed that -- fs_pbr_body.sh, fs_terrain.sc, fs_grass.sc all pass
    // v_viewdepth -- and the fourth, fs_foliage.sc, passed
    // distance(u_cameraPos.xyz, v_worldpos). Those differ by
    // 1/cos(angle from the view axis): at the corner of a 16:9 frame with a
    // 60-degree vertical fov that is a factor of 1.45.
    //
    // So a TREE and the TERRAIN BEHIND IT, at the same world position, were
    // given different amounts of fog -- agreeing at the screen centre,
    // diverging toward the edges, and shifting as the camera turned. A
    // silhouette with a visible fog step along it, moving with the view.
    //
    // Radial is also the physically correct one: aerial perspective integrates
    // along the path light travelled, which is the ray, not its projection
    // onto the camera's forward axis. Using view-space Z makes every fog
    // boundary a PLANE perpendicular to the view direction, which sweeps
    // across the world when the camera rotates on the spot; radial makes it a
    // sphere around the eye, which does not.
    //
    // Deriving it here from worldPos and cameraPos -- both already parameters
    // -- is what makes the four callers unable to disagree again. viewDepth is
    // kept in the signature for source compatibility and is now genuinely
    // UNREAD.
    //
    // It was briefly "consumed" by a `d + viewDepth * 0.0` to silence an
    // unused-parameter warning. That is a NaN hazard for nothing: x * 0.0 is
    // NaN when x is NaN or Inf, max(NaN, 0.0) is implementation-defined, and a
    // single poisoned fragment there takes the whole surface with it. A warning
    // is cheaper than that.
    vec3  ray     = worldPos - cameraPos;
    float rayLen  = max(length(ray), 1e-4);
    float d       = rayLen;

    // --- base distance factor (LINEAR / EXP / EXP2) ---
    float fLin  = clamp((d - start) / max(endd - start, 1e-4), 0.0, 1.0);
    float fExp  = clamp(1.0 - exp(-density * d), 0.0, 1.0);
    float x2    = density * d;
    float fExp2 = clamp(1.0 - exp(-(x2 * x2)), 0.0, 1.0);
    // Select by mode without `==` on a float (step buckets): 1,2,3.
    float f = fLin;
    if (mode > 1.5 && mode < 2.5) f = fExp;
    else if (mode > 2.5)          f = fExp2;

    // --- height attenuation (Wronski/UE closed form) ---
    // Ray from camera to fragment; vertical component drives the integral.
    float rayDirY = ray.y / rayLen;
    float hf      = u_fogColorSun.w;          // height falloff
    /* The world-Y datum the height integral is measured from.
     *
     * This was the literal 0.0, with this same comment calling it a "world Y
     * datum" -- the author knew it was data and then wrote a constant. The
     * scene field it should have read, fog_height_origin, is fully alive
     * everywhere else: four writers, a localized editor slider, JSON
     * round-trip, and it IS honoured by the volumetric path
     * (fs_volfog.sc reads it as u_volfog_p1.z). Only this path dropped it, and
     * only because the uniform budget above had no room left -- an accident of
     * packing that read as a decision.
     *
     * Default 0.0 keeps every existing scene byte-identical: the literal and
     * the data agree at the origin and diverge only as the world moves off
     * y = 0, which is why this survived review in a repository whose terrain
     * scenes mostly sit near it. */
    float h0      = u_fogHeight.x;             // height origin (world Y datum)
    // (1 - exp(-hf*rayDirY*d)) / (hf*rayDirY), with the rayDirY->0 limit == d.
    float ad      = hf * rayDirY;
    // The SAME length the result is normalised by on the hMul line below.
    // While `d` was view-space Z and rayLen was radial this was two different
    // quantities in one expression; now they are one, and the closed form
    // reduces to the true Wronski integral.
    float num     = 1.0 - exp(-ad * rayLen);
    // branchless-ish limit: blend the closed form toward `d` as |ad| -> 0.
    float safe    = (abs(ad) > 1e-5) ? (num / ad) : rayLen;
    float heightScale = exp(-hf * (cameraPos.y - h0)) * safe;
    // Normalize the height scale to [0,1] against the un-attenuated path
    // length so it modulates, not amplifies, the base factor.
    float hMul    = clamp(heightScale / rayLen, 0.0, 1.0);
    // Only let height attenuation reduce fog when hf > 0 (data default 0 => no
    // height effect, classic distance fog).
    f = (hf > 1e-5) ? (f * hMul) : f;

    // --- sun-side warm shift ---
    // viewDir = direction from camera toward the fragment.
    vec3 viewDir = ray / rayLen;
    // sunDirToLight points from fragment toward the sun; looking toward the
    // sun => dot(viewDir, sunDirToLight) high => warm fog tint.
    float sunAmt = pow(max(dot(viewDir, sunDirToLight), 0.0), 8.0);
    vec3  fogCol = mix(u_fogColor.rgb, u_fogColorSun.rgb, sunAmt);

    float strength = u_fogColor.w;
    float fogT = clamp(f * strength, 0.0, 1.0);
    return mix(litLinear, fogCol, fogT);
}

#endif // FOG_APPLY_SH
