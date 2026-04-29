/*
 * jce_gpu_particles.h -- GPU-driven compute particle system.
 *
 * Sibling to jce_particles.h (CPU pool, ≤ thousands).  This module
 * runs the entire simulation in a compute shader and renders the
 * pool with a single instanced billboard draw, scaling cleanly to
 * 1 M+ concurrent particles on modern GPUs.
 *
 * High-level model:
 *   - One pool of `max_particles` slots, 64 bytes each.
 *   - Each frame the runtime feeds the GPU:
 *       * an update dispatch (one thread per slot),
 *       * an emit dispatch  (one thread per requested spawn),
 *       * a single instanced quad draw.
 *   - Emission is configured per-frame via JceGpuParticleEmitConfig;
 *     no need to manage per-emitter handles for the common case
 *     (one global "VFX system" feeds many call sites).
 *
 * Backend: bgfx compute (DX11/Vulkan/Metal/GL4/GLES3.1).  Falls back
 * gracefully on devices without compute by skipping every dispatch.
 *
 * Layer: Graphics (Layer 3) — public.
 */

#ifndef JCE_GPU_PARTICLES_H
#define JCE_GPU_PARTICLES_H

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceGpuParticleSystem JceGpuParticleSystem;
typedef struct JcePakArchive       JcePakArchive;

typedef struct {
    uint32_t             max_particles; /* pool capacity (rounded up to 64) */
    const JcePakArchive *pak;           /* loads cs/vs/fs particle shaders */
} JceGpuParticleSystemDesc;

/* Per-frame emission and simulation parameters.  All fields are
 * sampled once per jce_gpu_particles_update() call. */
typedef struct {
    uint32_t emit_count;        /* how many particles to spawn this frame */

    jce_vec3 origin;            /* spawn position (world) */
    jce_vec3 velocity_min;      /* uniform-random bounds */
    jce_vec3 velocity_max;

    float    lifetime_min;      /* seconds */
    float    lifetime_max;

    float    size_start;
    float    size_end;

    jce_vec4 color_start;       /* RGBA */
    jce_vec4 color_end;

    jce_vec3 gravity;           /* world m/s² applied each tick */
    float    damping;           /* 1/s linear velocity damping (0 = off) */
} JceGpuParticleEmitConfig;

JCE_API JceGpuParticleSystem *jce_gpu_particles_create(
    const JceGpuParticleSystemDesc *desc, jce_allocator_t alloc);

JCE_API void jce_gpu_particles_destroy(JceGpuParticleSystem *sys);

/* Returns true if the GPU exposes the compute capability and the
 * system was fully initialised; false means the runtime is in a
 * harmless no-op mode (every call below becomes a stub). */
JCE_API bool jce_gpu_particles_is_supported(const JceGpuParticleSystem *sys);

/* Submit one frame's simulation work.
 *   compute_view_id : bgfx view bound to the compute dispatches
 *                     (use a dedicated view; no draws). */
JCE_API void jce_gpu_particles_update(JceGpuParticleSystem    *sys,
                                       uint16_t                 compute_view_id,
                                       float                    dt,
                                       const JceGpuParticleEmitConfig *cfg);

/* Submit the instanced billboard draw for the entire pool to
 * `render_view_id`.  Dead slots collapse to degenerate vertices in
 * the vertex shader, so cost is fragment-bound by the alive count. */
JCE_API void jce_gpu_particles_render(JceGpuParticleSystem *sys,
                                       uint16_t              render_view_id);

/* Pool capacity (rounded). */
JCE_API uint32_t jce_gpu_particles_capacity(const JceGpuParticleSystem *sys);

JCE_EXTERN_C_END

#endif /* JCE_GPU_PARTICLES_H */
