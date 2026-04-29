/*
 * cs_particle_emit.sc -- GPU emission pass.
 *
 * Dispatched once per frame with `u_emit_count` threads.  Each
 * thread pops one slot off the head of the free-list ring buffer
 * (slots whose lifetime field is <= 0 are considered dead) and
 * writes a freshly-spawned particle there.
 *
 * Free-list discovery is done by linear probe within the workgroup
 * starting from a per-frame rotated base, falling back to NOP if the
 * pool is full.  Cheaper than maintaining a true free-list; works
 * great when emit_count <<< pool_size (which is the common case).
 *
 * Bound resources:
 *   b_particles     : RW 4-vec4 stride buffer (the particle pool)
 *   u_emit_params   : (emit_count, base_index, pool_size, dt)
 *   u_emit_origin   : (ox, oy, oz, frame_seed)
 *   u_emit_v_min    : initial linear velocity, min
 *   u_emit_v_max    : initial linear velocity, max
 *   u_emit_life_size: (life_min, life_max, size_start, size_end)
 *   u_emit_color    : initial colour
 */

#include <bgfx_compute.sh>

BUFFER_RW(b_particles, vec4, 0);

uniform vec4 u_emit_params;
uniform vec4 u_emit_origin;
uniform vec4 u_emit_v_min;
uniform vec4 u_emit_v_max;
uniform vec4 u_emit_life_size;
uniform vec4 u_emit_color;

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

vec3 hash33(vec3 p) {
    return vec3(hash11(p.x + 17.1), hash11(p.y + 47.7), hash11(p.z + 91.3));
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint thread = gl_GlobalInvocationID.x;
    uint emit_count = uint(u_emit_params.x);
    if (thread >= emit_count) {
        return;
    }

    uint pool_size  = uint(u_emit_params.z);
    uint base       = uint(u_emit_params.y) + thread;
    float dt        = u_emit_params.w;

    // Linear probe up to 32 slots looking for a dead one.
    uint slot = pool_size;
    for (uint i = 0u; i < 32u; ++i) {
        uint candidate = (base + i) % pool_size;
        vec4 vel_life = b_particles[candidate * 4u + 1u];
        if (vel_life.w <= 0.0) {
            slot = candidate;
            break;
        }
    }
    if (slot >= pool_size) {
        return; // pool exhausted this frame; drop the spawn
    }

    float seed_base = u_emit_origin.w + float(thread) * 0.6180339;
    vec3 r0 = hash33(vec3(seed_base, seed_base + 1.7, seed_base + 3.1));
    vec3 r1 = hash33(vec3(seed_base + 5.5, seed_base + 7.7, seed_base + 9.9));

    vec3 vel = mix(u_emit_v_min.xyz, u_emit_v_max.xyz, r0);
    float life = mix(u_emit_life_size.x, u_emit_life_size.y, r1.x);
    float size = u_emit_life_size.z;

    b_particles[slot * 4u + 0u] = vec4(u_emit_origin.xyz, 0.0);
    b_particles[slot * 4u + 1u] = vec4(vel, life);
    b_particles[slot * 4u + 2u] = u_emit_color;
    b_particles[slot * 4u + 3u] = vec4(size, seed_base, 0.0, 0.0);
}
