/*
 * jce_particles.h  CPU-based particle system.
 *
 * Emitters produce particles each frame; an update pass moves them,
 * applies forces, and kills expired ones.  Rendering uses the
 * existing mesh/texture pipeline — each alive particle contributes
 * a billboard quad (or custom mesh) batched into a single draw call.
 *
 * Designed for small–medium particle counts (thousands, not millions).
 * For GPU particle simulation, a future compute-shader backend can
 * replace the update step without changing the public API.
 *
 * Layer: Graphics (Layer 3 — optional subsystem, priority 150).
 */

#ifndef JCE_PARTICLES_H
#define JCE_PARTICLES_H


#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Emitter configuration                                               */
/* ================================================================== */

typedef struct {
    /* -- Spawn ---------------------------------------------------- */
    uint32_t max_particles;       /* pool capacity (default: 1024) */
    float    emit_rate;           /* particles per second */
    float    emit_burst;          /* one-shot burst count (0 = disabled) */

    /* -- Lifetime ------------------------------------------------- */
    float    lifetime_min;        /* seconds (default: 1.0) */
    float    lifetime_max;        /* seconds (default: 2.0) */

    /* -- Initial velocity ----------------------------------------- */
    jce_vec3 velocity_min;        /* min linear velocity */
    jce_vec3 velocity_max;        /* max linear velocity */

    /* -- Acceleration / gravity ----------------------------------- */
    jce_vec3 gravity;             /* per-emitter gravity override */

    /* -- Size ----------------------------------------------------- */
    float    size_start;          /* initial billboard size (default: 0.1) */
    float    size_end;            /* size at death (default: 0.0) */

    /* -- Color ---------------------------------------------------- */
    jce_vec4 color_start;         /* RGBA at birth (default: white) */
    jce_vec4 color_end;           /* RGBA at death (default: transparent) */

    /* -- Texture -------------------------------------------------- */
    JceTextureHandle texture;     /* billboard texture (INVALID = white) */

    /* -- World / local space -------------------------------------- */
    bool     world_space;         /* true = particles ignore emitter movement */
} JceParticleEmitterDesc;

/* ================================================================== */
/* Emitter handle                                                      */
/* ================================================================== */

typedef struct { uint32_t idx; } JceEmitterHandle;
#define JCE_EMITTER_INVALID ((JceEmitterHandle){ UINT32_MAX })

static inline bool jce_emitter_valid(JceEmitterHandle h) { return h.idx != UINT32_MAX; }

/* ================================================================== */
/* Particle system (manages all emitters)                              */
/* ================================================================== */

typedef struct JceParticleSystem JceParticleSystem;

JCE_API JceParticleSystem *jce_particles_create(jce_allocator_t alloc);
JCE_API void               jce_particles_destroy(JceParticleSystem *sys);

/* ================================================================== */
/* Emitter management                                                  */
/* ================================================================== */

JceEmitterHandle jce_particles_emitter_add(JceParticleSystem *sys,
                                           const JceParticleEmitterDesc *desc);
void             jce_particles_emitter_remove(JceParticleSystem *sys,
                                              JceEmitterHandle emitter);

/* Start / stop emission.  Stopping lets existing particles live out
   their lifetime; remove kills them immediately. */
JCE_API void jce_particles_emitter_start(JceParticleSystem *sys, JceEmitterHandle emitter);
JCE_API void jce_particles_emitter_stop(JceParticleSystem *sys, JceEmitterHandle emitter);

/* Move the emitter origin (only matters when world_space = false). */
void jce_particles_emitter_set_position(JceParticleSystem *sys,
                                        JceEmitterHandle emitter, jce_vec3 pos);

/* Fire a one-shot burst of count particles. */
void jce_particles_emitter_burst(JceParticleSystem *sys,
                                 JceEmitterHandle emitter, uint32_t count);

/* ================================================================== */
/* Per-frame update & render                                           */
/* ================================================================== */

/* Simulate all alive particles (spawn, move, age, kill). */
JCE_API void jce_particles_update(JceParticleSystem *sys, float dt);

/* Return total alive particle count across all emitters. */
JCE_API uint32_t jce_particles_alive_count(const JceParticleSystem *sys);

/* ================================================================== */
/* Read-back (for rendering / debug visualisation)                     */
/* ================================================================== */

/* A single alive particle, exposed read-only for renderers. */
typedef struct {
    jce_vec3 position;   /* world-space when emitter world_space, else local */
    jce_vec4 color;      /* current interpolated RGBA */
    float    size;       /* current interpolated billboard size */
} JceParticleView;

/* Visit every alive particle of one emitter (newest pool order).  The
 * callback receives a stable snapshot per particle; do not retain the
 * pointer past the call. */
typedef void (*JceParticleVisitFn)(const JceParticleView *p, void *user_data);

JCE_API void jce_particles_emitter_for_each(const JceParticleSystem *sys,
                                            JceEmitterHandle emitter,
                                            JceParticleVisitFn cb,
                                            void *user_data);

/* Per-emitter alive count (0 for invalid / dead emitter). */
JCE_API uint32_t jce_particles_emitter_alive_count(const JceParticleSystem *sys,
                                                   JceEmitterHandle emitter);

/* True when the emitter slot is currently allocated (added, not removed). */
JCE_API bool jce_particles_emitter_is_alive(const JceParticleSystem *sys,
                                            JceEmitterHandle emitter);

/* ================================================================== */
/* Asset I/O                                                           */
/* ================================================================== */

/* Populate `out` with sane defaults (matches the editor authoring panel). */
JCE_API void jce_particles_desc_default(JceParticleEmitterDesc *out);

/* Load a `*.particles.json` emitter description from disk (host/VFS aware).
 * `out` is filled with defaults first, then overlaid with file values, so a
 * partial document still yields a usable emitter.  The optional `texture_out`
 * receives the authored texture path (may be empty); pass NULL to ignore.
 * Returns false on missing file / parse error (out is left at defaults). */
JCE_API bool jce_particles_desc_load_json(const char *path,
                                          JceParticleEmitterDesc *out,
                                          char *texture_out, int texture_cap);

JCE_EXTERN_C_END

#endif /* JCE_PARTICLES_H */
