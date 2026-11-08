/*
 * cs_particle_update.sc -- GPU per-frame integration pass.
 *
 * Dispatched with one thread per particle slot.  Reads the slot,
 * advances its age, applies gravity + linear damping, integrates
 * position and lerps colour/size against the emitter's start/end
 * targets.  Slots whose age exceeds their lifetime are marked dead
 * by writing lifetime = 0 (so the next emit pass can recycle them)
 * and zero size (so the render pass discards them).
 *
 * Bound resources:
 *   b_particles      : RW 4-vec4 stride buffer (the particle pool)
 *   u_update_params  : (pool_size, dt, _, _)
 *   u_update_grav    : (gx, gy, gz, damping)
 *   u_size_lerp      : (size_start, size_end, _, _)
 *   u_color_start    : start colour (rgba)
 *   u_color_end      : end colour (rgba)
 */

#include <bgfx_compute.sh>
#include "particle_rain_surface.sh"
#include "particle_roof.sh"

BUFFER_RW(b_particles, vec4, 0);

uniform vec4 u_update_params;
uniform vec4 u_update_grav;
uniform vec4 u_size_lerp;
uniform vec4 u_color_start;
uniform vec4 u_color_end;

bool particle_rain_hit(vec3 previous, vec3 next, vec3 velocity, out vec4 contact)
{
    contact = vec4_splat(0.0);
    if (!particle_rain_enabled() || !(next.y < previous.y)) return false;
    float y0, y1;
    if (!particle_rain_surface(previous.xz, y0) ||
        !particle_rain_surface(next.xz, y1)) return false;
    float before = previous.y - y0;
    float after = next.y - y1;
    if (!(before > 0.0 && after <= 0.0)) return false;
    float step_fraction = before / (before - after);
    vec3 hit = mix(previous, next, step_fraction);
    float surface_y;
    if (!particle_rain_surface(hit.xz, surface_y)) return false;
    hit.y = surface_y;
    if (particle_roof_blocks(hit, velocity)) return false;
    contact = vec4(hit, step_fraction);
    return true;
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint id = gl_GlobalInvocationID.x;
    uint pool_size = uint(u_update_params.x);
    if (id >= pool_size) {
        return;
    }

    uint base = id * 4u;
    vec4 pos_age  = b_particles[base + 0u];
    vec4 vel_life = b_particles[base + 1u];

    if (vel_life.w <= 0.0) {
        return; // already dead
    }

    float dt = u_update_params.y;
    float new_age = pos_age.w + dt;
    vec4 sz_seed = b_particles[base + 3u];
    bool ring = sz_seed.z > 0.5;

    if (new_age >= vel_life.w || (ring && !particle_rain_enabled())) {
        // Mark dead — emit pass recycles via lifetime <= 0.
        b_particles[base + 1u] = vec4(0.0, 0.0, 0.0, 0.0);
        b_particles[base + 3u] = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    if (ring) {
        b_particles[base + 0u] = vec4(pos_age.xyz, new_age);
        return;
    }

    vec3 vel = vel_life.xyz + u_update_grav.xyz * dt;
    vel *= max(0.0, 1.0 - u_update_grav.w * dt); // linear damping
    vec3 pos = pos_age.xyz + vel * dt;

    vec4 contact;
    if (particle_rain_hit(pos_age.xyz, pos, vel, contact)) {
        // Reuse the slot and retain elapsed time after contact within this step.
        float age = dt * (1.0 - contact.w);
        b_particles[base + 0u] = vec4(contact.xyz, age);
        b_particles[base + 1u] = vec4(vel, 0.38);
        b_particles[base + 2u] = vec4(u_color_start.rgb, clamp(-vel.y * 0.014, 0.07, 0.22));
        b_particles[base + 3u] = vec4(1.0, sz_seed.y, 1.0, 0.0);
        return;
    }

    float t   = new_age / vel_life.w;
    float sz  = mix(u_size_lerp.x, u_size_lerp.y, t);
    vec4  col = mix(u_color_start, u_color_end, t);

    b_particles[base + 0u] = vec4(pos, new_age);
    b_particles[base + 1u] = vec4(vel, vel_life.w);
    b_particles[base + 2u] = col;
    sz_seed.x = sz;
    b_particles[base + 3u] = sz_seed;
}
