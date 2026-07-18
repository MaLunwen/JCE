$input  a_position, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_texcoord0, v_pcolor

/*
 * vs_particle.sc -- view-aligned billboard expansion for GPU particles.
 *
 * One quad (4 vertices) is dispatched per draw, instanced once per
 * particle slot.  i_data0..3 carry the particle state directly out of
 * the compute pool.  Dead slots (size == 0) collapse to a degenerate
 * point so the rasteriser discards them with no fragment work.
 *
 * i_data1.xyz optionally carries velocity*stretch (world) for motion-aligned
 * streaks (rain / sparks).  It is honoured ONLY when u_particle_misc.y > 0.5
 * (the CPU scene path) AND the vector is non-zero, so every round emitter and
 * the GPU pool (which reuses i_data1 for vel/life) are unaffected.
 */

#include <bgfx_shader.sh>

uniform vec4 u_particle_misc;   // .y = 1 -> allow motion-stretch

void main()
{
    vec3  center = i_data0.xyz;
    float size   = i_data3.x;

    if (size <= 0.0) {
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        v_pcolor    = vec4(0.0, 0.0, 0.0, 0.0);
        v_texcoord0 = vec2(0.0, 0.0);
    } else {
        /* mul() — NOT raw m[i][j] indexing: the latter reads the matrix
         * transposed on GLSL vs HLSL, giving a wrong (skewed) billboard basis
         * on OpenGL that smears sprites into streaks at diagonal view angles.
         * mul(M, axis) extracts the world-space camera axis on every backend. */
        vec3 cam_right = mul(u_invView, vec4(1.0, 0.0, 0.0, 0.0)).xyz;
        vec3 cam_up    = mul(u_invView, vec4(0.0, 1.0, 0.0, 0.0)).xyz;

        vec3  stretch_vec = i_data1.xyz;
        float slen = length(stretch_vec);
        vec3  world;
        if (u_particle_misc.y > 0.5 && slen > 0.0001) {
            /* Motion-aligned streak: the billboard's long axis follows the
             * world velocity, its short axis stays perpendicular and faces the
             * camera.  Length = size + streak (a slow drop stays ~round, a fast
             * one elongates); width stays `size`.  The procedural soft circle in
             * fs_particle then reads as a soft vertical rain streak. */
            vec3  cam_pos  = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
            vec3  view_dir = normalize(cam_pos - center);
            vec3  up_axis  = stretch_vec / slen;
            vec3  rt_axis  = cross(up_axis, view_dir);
            float rl = length(rt_axis);
            rt_axis  = (rl > 0.0001) ? (rt_axis / rl) : cam_right;
            world = center + rt_axis * (a_position.x * size)
                           + up_axis * (a_position.y * (size + slen));
        } else {
            vec2 corner = a_position.xy * size;
            world = center + cam_right * corner.x + cam_up * corner.y;
        }

        gl_Position = mul(u_viewProj, vec4(world, 1.0));
        v_pcolor    = i_data2;
        v_texcoord0 = a_texcoord0;
    }
}
