/*
 * cs_particle_reset.sc -- GPU pool zero-fill (one-shot, at system creation).
 *
 * The pool is a COMPUTE_READ_WRITE dynamic VB; zero-filling it with a CPU
 * bgfx_update_dynamic_vertex_buffer crashes the D3D12 backend
 * (BufferD3D12::update on the renderer thread — same Upload-heap staging
 * hazard documented in cs_cull_reset.sc / jce_gpu_scene.c).  Clearing on the
 * GPU instead keeps the buffer's lifetime entirely device-side.  Dispatched
 * once before the first update/emit; the emit pass then sees lifetime <= 0
 * in every slot exactly as the old CPU memset intended.
 *
 * Bound resources (mirrors cs_particle_update):
 *   b_particles     : RW 4-vec4 stride buffer (the particle pool)
 *   u_update_params : (pool_size, _, _, _)
 */

#include <bgfx_compute.sh>

BUFFER_RW(b_particles, vec4, 0);

uniform vec4 u_update_params;

NUM_THREADS(64, 1, 1)
void main()
{
    uint id = gl_GlobalInvocationID.x;
    uint pool_size = uint(u_update_params.x);
    if (id >= pool_size) {
        return;
    }

    uint base = id * 4u;
    b_particles[base + 0u] = vec4(0.0, 0.0, 0.0, 0.0);
    b_particles[base + 1u] = vec4(0.0, 0.0, 0.0, 0.0);
    b_particles[base + 2u] = vec4(0.0, 0.0, 0.0, 0.0);
    b_particles[base + 3u] = vec4(0.0, 0.0, 0.0, 0.0);
}
