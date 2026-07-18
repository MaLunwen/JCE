$input v_texcoord0, v_normal, v_worldpos

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

SAMPLER2D(s_foliageAlpha, 0);

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

    // Boundary/aerial fog (linear; no v_viewdepth varying here, so use the
    // camera-to-fragment distance — equivalent for the fog integral).
    color = apply_aerial_fog(color, distance(u_cameraPos.xyz, v_worldpos),
                             v_worldpos, u_cameraPos.xyz,
                             normalize(u_foliage_light.xyz));

    gl_FragColor = vec4(color, 1.0);
}
