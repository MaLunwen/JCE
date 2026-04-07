$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    /* Fullscreen NDC quad — place at depth 1.0 (far plane background).
     * a_position.xy carries the NDC [-1, 1] corner positions.
     * We output those directly as v_texcoord0 for the fragment to
     * reconstruct the world-space view direction via u_invViewProj. */
    gl_Position = vec4(a_position.xy, 1.0, 1.0);
    v_texcoord0 = a_position.xy;
}
