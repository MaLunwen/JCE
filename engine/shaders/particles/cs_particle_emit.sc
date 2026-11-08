/*
 * cs_particle_emit.sc -- GPU emission pass.
 *
 * Dispatched once per frame with `u_emit_count` threads.  Each
 * thread consumes one unique slot from the GPU-compacted free list
 * (slots whose lifetime field is <= 0 are considered dead) and
 * writes a freshly-spawned particle there.
 *
 * A prior GPU scan appends each dead slot exactly once. Each invocation
 * consumes a unique list entry; no lifetime test-and-write reservation race.
 *
 * Bound resources:
 *   b_particles     : RW 4-vec4 stride buffer (the particle pool)
 *   b_free_slots    : unique dead pool indices produced by the scan
 *   b_counts        : (actual free count, actual accepted emissions)
 *   u_emit_params   : (emit_count, reserved, pool_size, dt)
 *   u_emit_origin   : (ox, oy, oz, frame_seed)
 *   u_emit_box      : full world-space box widths (zero = point emission)
 *   u_emit_v_min    : initial linear velocity, min
 *   u_emit_v_max    : initial linear velocity, max
 *   u_emit_life_size: (life_min, life_max, size_start, size_end)
 *   u_emit_color    : initial colour
 */

#include <bgfx_compute.sh>

BUFFER_RW(b_particles, vec4, 0);
BUFFER_RO(b_free_slots, uint, 1);
BUFFER_RW(b_counts, uint, 2);

uniform vec4 u_emit_params;
uniform vec4 u_emit_origin;
uniform vec4 u_emit_box;
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

    uint pool_size = uint(u_emit_params.z);
    uint free_count = b_counts[0];
    if (thread >= free_count || thread >= pool_size) return;
    uint slot = b_free_slots[thread];
    if (slot >= pool_size) return;

    float seed_base = u_emit_origin.w + float(thread) * 0.6180339;
    vec3 r0 = hash33(vec3(seed_base, seed_base + 1.7, seed_base + 3.1));
    vec3 r1 = hash33(vec3(seed_base + 5.5, seed_base + 7.7, seed_base + 9.9));

    vec3 vel = mix(u_emit_v_min.xyz, u_emit_v_max.xyz, r0);
    float life = mix(u_emit_life_size.x, u_emit_life_size.y, r1.x);
    if (!(life > 0.0)) return;
    float size = u_emit_life_size.z;

    vec3 position = u_emit_origin.xyz;
    if (u_emit_box.x > 0.0 || u_emit_box.y > 0.0 || u_emit_box.z > 0.0) {
        vec3 box_random = hash33(vec3(seed_base + 11.3, seed_base + 13.7, seed_base + 19.1));
        position += (box_random - vec3_splat(0.5)) * u_emit_box.xyz;
    }
    b_particles[slot * 4u + 0u] = vec4(position, 0.0);
    b_particles[slot * 4u + 1u] = vec4(vel, life);
    b_particles[slot * 4u + 2u] = u_emit_color;
    b_particles[slot * 4u + 3u] = vec4(size, seed_base, 0.0, 0.0);
    uint ignored;
    atomicFetchAndAdd(b_counts[1], 1u, ignored);
}
