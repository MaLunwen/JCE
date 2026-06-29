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

    // viewDepth is the positive view-space distance (v_viewdepth).
    float d = max(viewDepth, 0.0);

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
    vec3  ray     = worldPos - cameraPos;
    float rayLen  = max(length(ray), 1e-4);
    float rayDirY = ray.y / rayLen;
    float hf      = u_fogColorSun.w;          // height falloff
    float h0      = 0.0;                       // height origin (world Y datum)
    // (1 - exp(-hf*rayDirY*d)) / (hf*rayDirY), with the rayDirY->0 limit == d.
    float ad      = hf * rayDirY;
    float num     = 1.0 - exp(-ad * d);
    // branchless-ish limit: blend the closed form toward `d` as |ad| -> 0.
    float safe    = (abs(ad) > 1e-5) ? (num / ad) : d;
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
