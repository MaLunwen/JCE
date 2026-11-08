/* Included in the vertex main() to preserve the stock instruction order.
 * Callers supply center, size, and the two layout-specific expressions. */
        /* mul() — NOT raw m[i][j] indexing: the latter reads the matrix
         * transposed on GLSL vs HLSL, giving a wrong (skewed) billboard basis
         * on OpenGL that smears sprites into streaks at diagonal view angles.
         * mul(M, axis) extracts the world-space camera axis on every backend. */
        vec3 cam_right = mul(u_invView, vec4(1.0, 0.0, 0.0, 0.0)).xyz;
        vec3 cam_up    = mul(u_invView, vec4(0.0, 1.0, 0.0, 0.0)).xyz;

        vec3  stretch_vec = JCE_PARTICLE_STRETCH_VECTOR;
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
            world = center + rt_axis * (JCE_PARTICLE_CORNER.x * size)
                           + up_axis * (JCE_PARTICLE_CORNER.y * (size + slen));
        } else {
            vec2 corner = JCE_PARTICLE_CORNER.xy * size;
            world = center + cam_right * corner.x + cam_up * corner.y;
        }

        gl_Position = mul(u_viewProj, vec4(world, 1.0));
