$input v_texcoord0

#include <bgfx_shader.sh>

/* Sky gradient colors:
 *   [0] = top     (zenith)
 *   [1] = horizon
 *   [2] = ground
 *
 * Each element is a vec4; only .rgb is used (alpha ignored).
 * Default values are set on the CPU side before each submit. */
uniform vec4 u_sky_colors[3];

void main()
{
    /* Reconstruct world-space view ray direction.
     *
     * Unproject the NDC position at the near plane and far plane into
     * world space using bgfx's built-in u_invViewProj, then take the
     * normalized difference.  This gives a stable direction independent
     * of camera position, matching EditorOverlayRenderer.java. */
    vec4 nearH = mul(u_invViewProj, vec4(v_texcoord0, -1.0, 1.0));
    vec4 farH  = mul(u_invViewProj, vec4(v_texcoord0,  1.0, 1.0));
    vec3 dir   = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);

    /* Map direction.y in [-1, 1] to a sky/horizon/ground gradient.
     * Branch-free: sky_col blends horizon→top when dir.y > 0,
     *              gnd_col blends horizon→ground when dir.y < 0. */
    float t        = clamp(dir.y, -1.0, 1.0);
    vec3 sky_col   = mix(u_sky_colors[1].rgb, u_sky_colors[0].rgb, clamp( t, 0.0, 1.0));
    vec3 gnd_col   = mix(u_sky_colors[1].rgb, u_sky_colors[2].rgb, clamp(-t, 0.0, 1.0));
    float above    = step(0.0, t);   /* 1.0 if t >= 0, else 0.0 */
    vec3 col       = mix(gnd_col, sky_col, above);

    gl_FragColor = vec4(col, 1.0);
}
