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
#include <jce/os/core/jce_easing.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Particle events & sub-emitters (FEATURE 8.2)                        */
/* ================================================================== */

/* What happened to a particle when an event fires. */
typedef enum {
    JCE_PARTICLE_EVENT_BIRTH     = 0,  /* a particle was just spawned */
    JCE_PARTICLE_EVENT_DEATH     = 1,  /* a particle reached its lifetime */
    JCE_PARTICLE_EVENT_COLLISION = 2   /* a particle's collision flag tripped */
} JceParticleEventType;

/* Snapshot of one particle event handed to a sink.  All fields are valid
 * for every event type; do not retain the pointer past the callback. */
typedef struct {
    JceParticleEventType type;       /* what happened */
    jce_vec3             position;    /* particle position at the event */
    jce_vec3             velocity;    /* particle velocity at the event */
} JceParticleEvent;

/* Per-emitter event sink.  Invoked synchronously from the simulation step
 * (jce_particles_update) on the same thread that stepped the emitter; keep
 * it short and re-entrancy-free.  Set via jce_particles_emitter_set_sink. */
typedef void (*JceParticleEventFn)(const JceParticleEvent *ev, void *user_data);

/* Hard caps that bound sub-emitter spawn chains so a recursive / cyclic
 * sub-emitter graph can never produce a spawn storm. */
#define JCE_PARTICLE_SUBEMITTER_MAX_DEPTH 4u   /* parent->child chain depth */

/* ================================================================== */
/* Emitter configuration                                               */
/* ================================================================== */

typedef struct JceParticleEmitterDesc {
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

    /* -- Per-property lifetime curves (FEATURE 8.3) --------------- *
     * Each curve shapes how a property travels from its start value to
     * its end value across a particle's normalised age t in [0,1].  The
     * default (JCE_EASE_LINEAR == 0) reproduces the legacy pure-linear
     * interpolation byte-for-byte, so a zero-initialised desc — or any
     * desc that never sets these — behaves exactly as before.
     *
     *   value(t) = start + (end - start) * jce_ease(curve, t)
     *
     * velocity_curve additionally scales the integrated velocity by a
     * curve over age (1.0 at t=0 by convention) so emitters can ramp
     * particle motion in / out without touching gravity. */
    JceEaseType size_curve;       /* size_start -> size_end shaping (default LINEAR) */
    JceEaseType color_curve;      /* color_start -> color_end shaping (default LINEAR) */
    JceEaseType velocity_curve;   /* velocity-scale-over-age shaping (default LINEAR) */
    float       velocity_scale_start; /* multiplier at birth  (default 1.0) */
    float       velocity_scale_end;   /* multiplier at death  (default 1.0) */

    /* -- Texture -------------------------------------------------- */
    JceTextureHandle texture;     /* billboard texture (INVALID = white) */

    /* -- Flipbook / texture-sheet animation (FEATURE 8.3) --------- *
     * Treat `texture` as an atlas of flipbook_rows x flipbook_cols equal
     * cells played over a particle's life.  When flipbook_rows and
     * flipbook_cols are both >= 1 and their product > 1 the system
     * advances a frame index by age and exposes the active cell's UV
     * sub-rect on JceParticleView (uv_offset / uv_scale).  Leave the
     * counts at 0 (the default) to disable flipbook entirely: the view
     * then reports the full [0,0]-[1,1] rect and is byte-identical to
     * today.
     *
     * Frame selection:
     *   - flipbook_fps > 0  : frame = floor(age * fps), time-driven.
     *   - flipbook_fps == 0 : the whole sheet plays exactly once over the
     *                         particle lifetime (frames-over-life).
     * flipbook_loop controls out-of-range frames: true wraps (modulo),
     * false clamps to the last frame. */
    uint32_t flipbook_rows;       /* atlas rows    (0 = no flipbook) */
    uint32_t flipbook_cols;       /* atlas columns (0 = no flipbook) */
    float    flipbook_fps;        /* frames/sec (0 = play once over life) */
    bool     flipbook_loop;       /* true = wrap frames, false = clamp */

    /* -- World / local space -------------------------------------- */
    bool     world_space;         /* true = particles ignore emitter movement */

    /* -- Sub-emitters (FEATURE 8.2) -------------------------------- *
     * A child emitter description spawned at a parent particle's
     * position.  `sub_emitter` is borrowed (the caller owns the memory
     * and must keep it alive for the parent emitter's lifetime); leave
     * it NULL for the common no-sub-emitter case — such emitters stay
     * byte-for-byte identical to the legacy behaviour.
     *
     * On a triggering event the child spawns N particles at the parent
     * particle's position, inheriting nothing else (it uses its own
     * desc's velocity / lifetime / colour, etc.).  Depth is bounded by
     * JCE_PARTICLE_SUBEMITTER_MAX_DEPTH to prevent recursion storms. */
    const struct JceParticleEmitterDesc *sub_emitter; /* borrowed child desc (NULL = none) */
    uint32_t sub_spawn_on_birth;  /* child particles spawned per parent birth (0 = none) */
    uint32_t sub_spawn_on_death;  /* child particles spawned per parent death (0 = none) */
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
/* Particle events (FEATURE 8.2)                                       */
/* ================================================================== */

/* Register (or clear, with cb=NULL) the event sink for one emitter.  The
 * sink is invoked from jce_particles_update for every particle BIRTH /
 * DEATH (and COLLISION when the collision flag is raised — see
 * jce_particles_emitter_flag_collision).  user_data is passed through
 * unmodified.  Returns nothing; a no-sink emitter behaves exactly as before. */
JCE_API void jce_particles_emitter_set_sink(JceParticleSystem *sys,
                                            JceEmitterHandle emitter,
                                            JceParticleEventFn cb,
                                            void *user_data);

/* Mark the alive particle at index `particle_idx` (pool order, as visited by
 * jce_particles_emitter_for_each) as collided.  On the next update step the
 * particle is killed, fires a COLLISION event (not a DEATH event), and runs
 * the parent's on-death sub-emitter spawn.  No-op for invalid args. */
JCE_API void jce_particles_emitter_flag_collision(JceParticleSystem *sys,
                                                  JceEmitterHandle emitter,
                                                  uint32_t particle_idx);

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

    /* Flipbook UV sub-rect (FEATURE 8.3).  The renderer remaps a quad's
     * [0,1] texcoords into this rect: uv' = uv_offset + uv * uv_scale.
     * With no flipbook configured this is the identity rect
     * (offset = {0,0}, scale = {1,1}), so existing renderers are
     * unaffected. */
    jce_vec2 uv_offset;  /* top-left UV of the active flipbook cell */
    jce_vec2 uv_scale;   /* per-axis UV extent of one cell (1/cols, 1/rows) */
    uint32_t frame;      /* active flipbook frame index (0 when disabled) */
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

/* Release any loader-owned heap data attached to a desc by
 * jce_particles_desc_load_json — currently the nested sub-emitter child desc
 * (FEATURE 8.2).  Safe to call on any desc: it frees out->sub_emitter only
 * when non-NULL and resets the pointer, so a desc filled by
 * jce_particles_desc_default (sub_emitter == NULL) is a no-op.  Call after the
 * desc has been consumed by jce_particles_emitter_add (which deep-copies the
 * child synchronously).  Does NOT free `out` itself. */
JCE_API void jce_particles_desc_free(JceParticleEmitterDesc *out);

JCE_EXTERN_C_END

#endif /* JCE_PARTICLES_H */
