/*
 * jce_particle_modules.h  Sample particle-emitter module values.
 *
 * Pure CPU helpers that consume B7.8's JceParticleEmitterComponent
 * and return per-particle values at a given normalised lifetime
 * `t ∈ [0, 1]`.  Renderer-side particle systems call into these to
 * resolve color / size / velocity overrides without re-implementing
 * the gradient / curve evaluators in every module.
 *
 * Layer: middleware / scene (Layer 4) — public.
 */

#ifndef JCE_PARTICLE_MODULES_H
#define JCE_PARTICLE_MODULES_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Sample the color-over-lifetime gradient at `t ∈ [0,1]`.  Out
 * receives RGBA in linear space.  Returns false (no write) when the
 * module is disabled or the stop list is empty. */
JCE_API bool jce_particle_sample_color_over_lifetime(
    const JceParticleEmitterComponent *emitter,
    float                              t,
    float                              out_rgba[4]);

/* Sample the size-over-lifetime curve at `t ∈ [0,1]`.  Out receives
 * a multiplier the caller applies to its base size.  Returns false
 * when the module is disabled or the curve is empty. */
JCE_API bool jce_particle_sample_size_over_lifetime(
    const JceParticleEmitterComponent *emitter,
    float                              t,
    float                             *out_size_mul);

/* Generate an initial spawn position inside the emitter's shape.
 * `random01_x/y/z` are caller-supplied uniform [0,1] randoms — the
 * helper converts them deterministically into shape-respecting
 * positions so the same seed yields the same spawn.  Output is in
 * emitter-local space; caller transforms to world. */
JCE_API jce_vec3 jce_particle_sample_spawn_position(
    const JceParticleEmitterComponent *emitter,
    float                              random01_x,
    float                              random01_y,
    float                              random01_z);

/* Initial velocity for a freshly-spawned particle, given the spawn
 * position (so cones/spheres can produce outward-facing velocity).
 * When velocity_over_lifetime_enabled is false this returns the
 * authored "initial direction" (zero vector by default for shapes
 * that don't imply motion). */
JCE_API jce_vec3 jce_particle_sample_initial_velocity(
    const JceParticleEmitterComponent *emitter,
    jce_vec3                           spawn_local);

JCE_EXTERN_C_END

#endif /* JCE_PARTICLE_MODULES_H */
