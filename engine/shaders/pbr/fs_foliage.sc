$input v_texcoord0, v_normal, v_worldpos, v_localpos
// v_localpos is unused here and MUST still be declared: vs_foliage.sc is
// shared with fs_foliage_shadow.sc, which needs object space for hashed
// alpha, and a fragment shader whose $input set does not match the vertex
// shader's $output set produces an INVALID PROGRAM -- the foliage then
// does not draw at all, and the shadow pass silently falls back to its
// sphere-proxy blobs.  Adding an output to a shared VS is a change to
// every FS paired with it.

#include <bgfx_shader.sh>

/*
 * fs_foliage.sc -- 3-tone toon foliage card (paired with vs_foliage.sc).
 *
 * Unlit by design: the authored ramp colors ARE the lighting for the
 * current look (season x time-of-day) -- exactly the reference model.
 *   ramp key t = clamp(worldY * 0.1 + ndl, 0, 1)
 *   ndl        = dot(shellNormal, toLight) * 0.6 + 0.4
 *   color      = ramp(shadow, mid, highlight)(t) * multiplier
 * Leaf silhouette from the alpha mask (smoothstep .4-.6, discard < .01).
 */

uniform vec4 u_foliage_colors[4];   // [0]=shadow [1]=mid [2]=highlight [3]=multiplier
uniform vec4 u_foliage_light;       // xyz = TO-light direction (normalized)
uniform vec4 u_cameraPos;           // xyz = world camera position (PBR global bind)

// Aerial fog (uniforms ride the PBR global bind issued before the foliage
// submit; mode NONE gate = bit-exact no-op).  The reference fogs bushes and
// tree canopies too (three.js scene.fog) — without it the far tree line
// stayed saturated against the fogged ground and the world edge never
// dissolved into the boundary mist.
#include "fog_apply.sh"
#include "shadow_debug.sh"

/* Both ride the PBR global bind issued before the foliage submit, same as
 * u_cameraPos above.  Declared here because this shader deliberately does not
 * include fs_pbr_body.sh -- foliage is a separate, stylised shading model. */
uniform vec4 u_normalScale;   // z = JceSceneViewModeKind
uniform vec4 u_csmSplits;     // w = shadow far plane (debug depth normaliser)

SAMPLER2D(s_foliageAlpha, 0);
SAMPLER2D(s_cloudShadow, 3);
uniform vec4 u_cloudShadow;   // x=extent  yz=centre XZ  w=strength
#include "cloud_shadow.sh"

vec3 fol_ramp(float t, vec3 s, vec3 m, vec3 h)
{
    if (t < 0.5) return mix(s, m, t * 2.0);
    return mix(m, h, (t - 0.5) * 2.0);
}

void main()
{
    float alpha = texture2D(s_foliageAlpha, v_texcoord0).a;
    float a = smoothstep(0.4, 0.6, alpha);
    if (a < 0.01) discard;

    vec3 n = normalize(v_normal);
    float ndl = dot(n, normalize(u_foliage_light.xyz));
    ndl = ndl * 0.6 + 0.4;

    float t = clamp(v_worldpos.y * 0.1 + ndl, 0.0, 1.0);
    vec3 color = fol_ramp(t, u_foliage_colors[0].rgb,
                             u_foliage_colors[1].rgb,
                             u_foliage_colors[2].rgb);
    color *= u_foliage_colors[3].rgb;

    /* The only occluder this shader has.
     *
     * Foliage samples no shadow map at all -- see the SHADOW_MASK note below,
     * which reports 1.0 truthfully for exactly that reason -- so a canopy has
     * never been in shade of any kind. A cloud shadow needs no shadow map: it
     * is a top-down transmittance sampled by world XZ, and it is the one
     * occlusion term this shading model can honestly carry.
     *
     * Applied to the whole ramp rather than to a direct-light term, because
     * there is no separable direct-light term here: the ramp IS the shading
     * model. That makes it slightly stronger on foliage than on the surfaces
     * that multiply only their sun contribution, which is the right way round
     * for a canopy standing in the open. */
    color *= cloud_shadow_at(v_worldpos.xz);

    // Boundary/aerial fog (linear; no v_viewdepth varying here, so use the
    // camera-to-fragment distance — equivalent for the fog integral).
    color = apply_aerial_fog(color, distance(u_cameraPos.xyz, v_worldpos),
                             v_worldpos, u_cameraPos.xyz,
                             normalize(u_foliage_light.xyz));

    /* Debug views.  Foliage had honoured NO view mode at all since the
     * material channels were added -- it is a separate shading model and was
     * simply never wired -- so every debug view was blank on the vegetation,
     * which in a forest scene is most of what anyone is looking at. That gap
     * is why a distance measurement taken through these views covered only
     * 39.7% of the viewport and excluded, precisely, the trees.
     *
     * SHADOW_MASK reports 1.0 truthfully: this shader samples no shadow map,
     * so foliage is never in cast shadow.
     *
     * SHADOW_CASCADES reports MAGENTA, not the "past all cascades" grey. Grey
     * would be a lie of the most useful-looking kind -- it would read as "this
     * fragment is beyond the shadow range", inviting someone to go lengthen
     * the range, when the truth is that this shader never asks about cascades
     * at any distance. Magenta appears in no cascade palette, so it cannot be
     * mistaken for an answer.
     *
     * SCENE_DEPTH uses the camera-to-fragment distance, because there is no
     * v_viewdepth varying here; it is radial where the PBR path is view-space
     * Z. They agree at the centre of the frame and diverge toward the edges,
     * which is also true of the fog above and is noted rather than hidden. */
    float viewMode = u_normalScale.z;
    if (viewMode > 7.5)
    {
        vec3 dbg;
        if      (viewMode < 8.5) dbg = shadow_debug_depth_color(
                                          distance(u_cameraPos.xyz, v_worldpos),
                                          u_csmSplits.w);
        else if (viewMode < 9.5) dbg = vec3(1.0, 0.0, 1.0);
        else                     dbg = vec3_splat(1.0);
        gl_FragColor = vec4(dbg, 1.0);
        return;
    }

    gl_FragColor = vec4(color, 1.0);
}
