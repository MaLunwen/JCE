$input  a_position, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_texcoord0, v_pcolor

/*
 * vs_particle.sc -- view-aligned billboard expansion for GPU particles.
 *
 * One quad (4 vertices) is dispatched per draw, instanced once per
 * particle slot.  i_data0..3 carry the particle state directly out of
 * the compute pool.  Dead slots (size == 0) collapse to a degenerate
 * point so the rasteriser discards them with no fragment work.
 */

#include <bgfx_shader.sh>

void main()
{
    vec3  center = i_data0.xyz;
    float size   = i_data3.x;

    if (size <= 0.0) {
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        v_pcolor    = vec4(0.0, 0.0, 0.0, 0.0);
        v_texcoord0 = vec2(0.0, 0.0);
    } else {
        vec3 cam_right = vec3(u_invView[0][0], u_invView[1][0], u_invView[2][0]);
        vec3 cam_up    = vec3(u_invView[0][1], u_invView[1][1], u_invView[2][1]);

        vec2 corner = a_position.xy * size;
        vec3 world  = center + cam_right * corner.x + cam_up * corner.y;

        gl_Position = mul(u_viewProj, vec4(world, 1.0));
        v_pcolor    = i_data2;
        v_texcoord0 = a_texcoord0;
    }
}
