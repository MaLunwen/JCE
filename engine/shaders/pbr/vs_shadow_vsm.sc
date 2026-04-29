$input  a_position
$output v_viewdepth

#include <bgfx_shader.sh>

/* VSM shadow vertex pass.
 *
 * Identical to vs_shadow but exports view-space depth (linear, positive
 * looking away from light) so the fragment shader can write the
 * (depth, depth^2) moments without re-deriving from gl_FragCoord. */
void main()
{
    vec4 wp  = mul(u_model[0], vec4(a_position, 1.0));
    vec4 vp  = mul(u_view,  wp);
    gl_Position = mul(u_proj, vp);

    /* Light is the active "camera" during the shadow pass; vp.z is
     * negative looking down -Z, so flip the sign for a positive
     * linear depth in [0, far]. */
    v_viewdepth = -vp.z;
}
