/*
 * jce_particle_force_fields.h  Spatial force fields for particles.
 *
 * Unity ParticleSystem.ForceOverLifetime + ExternalForcesModule
 * equivalents.  Four kinds:
 *   - External    : constant force in a direction (analog to wind)
 *   - Turbulence  : sin/cos lattice noise jitter
 *   - Drag        : opposes velocity (returns -velocity * coeff)
 *   - Vortex      : rotates around an axis through the origin
 *
 * The particle simulator (jce_particle_modules) queries
 * jce_particle_sample_force(world_pos, world_vel, time, out_force)
 * each step and adds the resulting vector to per-particle velocity.
 *
 * Layer: scene (Layer 4) — public.
 */

#ifndef JCE_PARTICLE_FORCE_FIELDS_H
#define JCE_PARTICLE_FORCE_FIELDS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_PARTICLE_FORCE_FIELD_MAX 32

typedef enum {
    JCE_PARTICLE_FORCE_EXTERNAL   = 0,
    JCE_PARTICLE_FORCE_TURBULENCE = 1,
    JCE_PARTICLE_FORCE_DRAG       = 2,
    JCE_PARTICLE_FORCE_VORTEX     = 3,
} JceParticleForceKind;

typedef enum {
    JCE_PARTICLE_FORCE_SHAPE_INFINITE = 0,  /* whole world */
    JCE_PARTICLE_FORCE_SHAPE_SPHERE   = 1,
    JCE_PARTICLE_FORCE_SHAPE_BOX      = 2,
} JceParticleForceShape;

typedef struct {
    JceParticleForceKind  kind;
    JceParticleForceShape shape;

    /* Shape parameters. */
    float center[3];
    float radius;          /* sphere */
    float half_extents[3]; /* box */

    /* Kind parameters. */
    float direction[3];    /* external + vortex axis */
    float strength;
    float frequency;       /* turbulence noise scale */
    float drag_coefficient;
    /* Per-axis amplitude (turbulence). */
    float turbulence_amp[3];

    bool  active;
} JceParticleForceField;

/* Registry. */
JCE_API void     jce_particle_force_clear(void);
JCE_API bool     jce_particle_force_register(const JceParticleForceField *ff);
JCE_API uint32_t jce_particle_force_count(void);

/* Sample summed force at `world_pos` (and current `world_vel` for
 * drag) at simulation time `t_seconds`. */
JCE_API void jce_particle_sample_force(const float world_pos[3],
                                         const float world_vel[3],
                                         float       t_seconds,
                                         float       out_force[3]);

JCE_EXTERN_C_END

#endif /* JCE_PARTICLE_FORCE_FIELDS_H */
