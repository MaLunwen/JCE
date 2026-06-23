/*
 * jce_particles.c  CPU-based particle system implementation.
 *
 * Each emitter owns a flat pool of particles.  Alive particles
 * are packed at the front; dead ones swap to the end (unstable sort)
 * for cache-friendly iteration.
 */

#include <jce/os/core/jce_easing.h>   /* per-property lifetime curves */
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_jobs.h>      /* parallel emitter update */
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_particles.h>

#include "os/core/jce_memory.h"   /* JCE_MALLOC / JCE_FREE (mimalloc-tracked) */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "particles"

#define MAX_EMITTERS 256

/* ── Single particle ───────────────────────────────────────────────── */

typedef struct {
    jce_vec3 position;
    jce_vec3 velocity;
    jce_vec4 color;
    float    size;
    float    age;
    float    lifetime;
    bool     collided;   /* collision flag raised externally → kill next step */

    /* -- Flipbook UV (FEATURE 8.3) -------------------------------- *
     * Cached active-cell sub-rect, recomputed from age each step when the
     * emitter has a flipbook configured.  Defaults to the identity rect so
     * non-flipbook particles report the full texture. */
    jce_vec2 uv_offset;
    jce_vec2 uv_scale;
    uint32_t frame;
} Particle;

/* ── Emitter ───────────────────────────────────────────────────────── */

typedef struct {
    bool                    alive;
    bool                    emitting;
    JceParticleEmitterDesc  desc;
    jce_vec3                origin;

    Particle               *pool;
    uint32_t                alive_count;

    float                   emit_accumulator;
    uint32_t                rng_state;   /* per-emitter xorshift32 (parallel-safe) */

    /* -- Events (FEATURE 8.2) ------------------------------------- */
    JceParticleEventFn      sink;        /* per-emitter event sink (NULL = none) */
    void                   *sink_user;   /* opaque sink user data */

    /* -- Sub-emitter (FEATURE 8.2) -------------------------------- *
     * When the parent desc carries a sub_emitter, a child Emitter is
     * created lazily into a sibling slot of the same system, and the
     * parent records its handle here.  `depth` bounds the chain. */
    JceEmitterHandle        sub_handle;  /* child emitter slot (INVALID = none) */
    uint32_t                depth;       /* 0 = top-level; bounds recursion */

    /* -- Deferred events (FEATURE 8.2) ---------------------------- *
     * Recorded during the (possibly parallel) update step writing ONLY
     * to this emitter's own buffer — no cross-emitter writes, so the
     * parallel step stays race-free.  A serial pass after the parallel
     * update drains these: it invokes the sink and applies sub-emitter
     * spawns into the (sibling) child pool. */
    JceParticleEvent       *events;      /* grown on demand (NULL when unused) */
    uint32_t                event_count;
    uint32_t                event_cap;
} Emitter;

/* ── System ────────────────────────────────────────────────────────── */

struct JceParticleSystem {
    jce_allocator_t alloc;
    Emitter         emitters[MAX_EMITTERS];
    uint32_t        emitter_count;
    uint32_t        rng_state;   /* xorshift32 */
};

/* ── Pseudo-RNG (xorshift32) ───────────────────────────────────────── */

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Random float in [0, 1). */
static float randf(uint32_t *state)
{
    return (float)(xorshift32(state) & 0x00FFFFFF) / (float)0x01000000;
}

/* Random float in [a, b]. */
static float randf_range(uint32_t *state, float a, float b)
{
    return a + randf(state) * (b - a);
}

/* ── Flipbook helpers (FEATURE 8.3) ────────────────────────────────── */

/* True when the emitter is configured for flipbook UV animation. */
static bool desc_has_flipbook(const JceParticleEmitterDesc *d)
{
    return d->flipbook_rows >= 1u && d->flipbook_cols >= 1u &&
           (uint64_t)d->flipbook_rows * (uint64_t)d->flipbook_cols > 1u;
}

/* Compute the active flipbook cell index + UV sub-rect for a particle of the
 * given age / lifetime.  Writes the identity rect (full texture, frame 0) when
 * the emitter has no flipbook configured, so the legacy path is unaffected. */
static void compute_flipbook_uv(const JceParticleEmitterDesc *d,
                                float age, float lifetime,
                                jce_vec2 *out_offset, jce_vec2 *out_scale,
                                uint32_t *out_frame)
{
    if (!desc_has_flipbook(d)) {
        *out_offset = jce_v2(0.0f, 0.0f);
        *out_scale  = jce_v2(1.0f, 1.0f);
        *out_frame  = 0u;
        return;
    }

    uint32_t cols   = d->flipbook_cols;
    uint32_t rows   = d->flipbook_rows;
    uint32_t total  = rows * cols;

    /* Frame index by time (fps) or stretched over the whole lifetime. */
    float fidx;
    if (d->flipbook_fps > 0.0f) {
        fidx = age * d->flipbook_fps;
    } else {
        float life = lifetime > 0.0f ? lifetime : 1.0f;
        float t    = age / life;            /* normalised age [0,1+] */
        fidx = t * (float)total;            /* sheet plays exactly once */
    }
    if (fidx < 0.0f) fidx = 0.0f;

    uint32_t frame = (uint32_t)fidx;        /* floor */
    if (d->flipbook_loop) {
        frame = frame % total;              /* wrap */
    } else if (frame >= total) {
        frame = total - 1u;                 /* clamp to last cell */
    }

    uint32_t col = frame % cols;
    uint32_t row = frame / cols;

    float du = 1.0f / (float)cols;
    float dv = 1.0f / (float)rows;

    *out_offset = jce_v2((float)col * du, (float)row * dv);
    *out_scale  = jce_v2(du, dv);
    *out_frame  = frame;
}

/* ── Create / Destroy ──────────────────────────────────────────────── */

JceParticleSystem *jce_particles_create(jce_allocator_t alloc)
{
    JceParticleSystem *sys = (JceParticleSystem *)alloc.alloc(
        sizeof(JceParticleSystem), alloc.ctx);
    if (!sys) return NULL;

    memset(sys, 0, sizeof(*sys));
    sys->alloc     = alloc;
    sys->rng_state = 0xDEADBEEFu;

    LOG_SUCCESS(LOG_TAG, "particle system created");
    return sys;
}

void jce_particles_destroy(JceParticleSystem *sys)
{
    if (!sys) return;

    for (uint32_t i = 0; i < MAX_EMITTERS; i++) {
        if (sys->emitters[i].pool)
            sys->alloc.free(sys->emitters[i].pool, sys->alloc.ctx);
        if (sys->emitters[i].events)
            sys->alloc.free(sys->emitters[i].events, sys->alloc.ctx);
    }
    sys->alloc.free(sys, sys->alloc.ctx);
    /* LOG after free is unsafe — omitted intentionally. */
}

/* ── Emitter management ────────────────────────────────────────────── */

/* Internal add carrying the sub-emitter recursion depth.  Public add is a
 * depth-0 wrapper.  Returns a handle; on success may recurse once to build
 * the child emitter referenced by desc->sub_emitter (depth-bounded). */
static JceEmitterHandle emitter_add_depth(JceParticleSystem *sys,
                                          const JceParticleEmitterDesc *desc,
                                          uint32_t depth)
{
    if (!sys || !desc) return JCE_EMITTER_INVALID;

    for (uint32_t i = 0; i < MAX_EMITTERS; i++) {
        if (!sys->emitters[i].alive) {
            Emitter *em = &sys->emitters[i];
            memset(em, 0, sizeof(*em));
            em->alive      = true;
            em->emitting   = true;
            em->desc       = *desc;
            em->origin     = jce_v3(0, 0, 0);
            em->sub_handle = JCE_EMITTER_INVALID;
            em->depth      = depth;

            uint32_t cap = desc->max_particles ? desc->max_particles : 1024;
            em->pool = (Particle *)sys->alloc.alloc(
                sizeof(Particle) * cap, sys->alloc.ctx);
            if (!em->pool) {
                em->alive = false;
                return JCE_EMITTER_INVALID;
            }
            memset(em->pool, 0, sizeof(Particle) * cap);
            em->desc.max_particles = cap;

            /* Distinct per-emitter seed (never 0 — xorshift32 stalls on 0)
             * so each emitter's randomness is independent → parallel-safe. */
            em->rng_state = ((uint32_t)i + 1u) * 2654435761u ^ 0x9E3779B9u;
            if (em->rng_state == 0u) em->rng_state = 0xDEADBEEFu;

            sys->emitter_count++;

            /* Build the child sub-emitter, depth-bounded.  The child never
             * free-runs: it is set non-emitting so it only produces particles
             * via parent-driven spawns.  Beyond the depth cap the chain is
             * silently truncated to prevent spawn storms. */
            if (desc->sub_emitter &&
                (desc->sub_spawn_on_birth || desc->sub_spawn_on_death) &&
                depth + 1u < JCE_PARTICLE_SUBEMITTER_MAX_DEPTH) {
                JceEmitterHandle ch =
                    emitter_add_depth(sys, desc->sub_emitter, depth + 1u);
                if (jce_emitter_valid(ch)) {
                    /* `em` may have moved relative to nothing (array slot is
                     * stable), but re-fetch to be explicit about the slot. */
                    sys->emitters[i].sub_handle = ch;
                    sys->emitters[ch.idx].emitting = false;
                } else {
                    LOG_WARN(LOG_TAG, "sub-emitter pool full; child skipped");
                }
            }

            return (JceEmitterHandle){ i };
        }
    }

    LOG_ERROR(LOG_TAG, "emitter limit reached (%u)", MAX_EMITTERS);
    return JCE_EMITTER_INVALID;
}

JceEmitterHandle jce_particles_emitter_add(JceParticleSystem *sys,
                                           const JceParticleEmitterDesc *desc)
{
    return emitter_add_depth(sys, desc, 0u);
}

void jce_particles_emitter_remove(JceParticleSystem *sys,
                                  JceEmitterHandle emitter)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;

    Emitter *em = &sys->emitters[emitter.idx];
    if (em->alive) {
        /* Reap the owned child sub-emitter first (recursive, depth-bounded). */
        if (jce_emitter_valid(em->sub_handle)) {
            JceEmitterHandle child = em->sub_handle;
            em->sub_handle = JCE_EMITTER_INVALID;
            jce_particles_emitter_remove(sys, child);
        }
        if (em->pool)
            sys->alloc.free(em->pool, sys->alloc.ctx);
        if (em->events)
            sys->alloc.free(em->events, sys->alloc.ctx);
        em->pool        = NULL;
        em->events      = NULL;
        em->event_count = 0;
        em->event_cap   = 0;
        em->sink        = NULL;
        em->sink_user   = NULL;
        em->alive       = false;
        sys->emitter_count--;
    }
}

void jce_particles_emitter_start(JceParticleSystem *sys, JceEmitterHandle emitter)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;
    Emitter *em = &sys->emitters[emitter.idx];
    if (em->alive) em->emitting = true;
}

void jce_particles_emitter_stop(JceParticleSystem *sys, JceEmitterHandle emitter)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;
    Emitter *em = &sys->emitters[emitter.idx];
    if (em->alive) em->emitting = false;
}

void jce_particles_emitter_set_position(JceParticleSystem *sys,
                                        JceEmitterHandle emitter, jce_vec3 pos)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;
    Emitter *em = &sys->emitters[emitter.idx];
    if (em->alive) em->origin = pos;
}

/* ── Deferred event recording (FEATURE 8.2) ────────────────────────── *
 *
 * Appends to the emitter's OWN event buffer only — no cross-emitter writes,
 * so it is safe to call from the parallel update step.  The buffer is drained
 * serially after the parallel pass (sink invoke + sub-emitter spawn).  Events
 * are only recorded when the emitter actually needs them (sink set or a
 * sub-emitter wired to the matching trigger); otherwise this is a no-op and
 * a sub-emitter-free / sink-free emitter stays byte-identical to today. */
static bool emitter_wants_event(const Emitter *em, JceParticleEventType type)
{
    if (em->sink) return true;
    if (!em->desc.sub_emitter || !jce_emitter_valid(em->sub_handle)) return false;
    if (type == JCE_PARTICLE_EVENT_BIRTH) return em->desc.sub_spawn_on_birth > 0;
    /* DEATH and COLLISION both drive the on-death sub-emitter spawn. */
    return em->desc.sub_spawn_on_death > 0;
}

static void record_event(JceParticleSystem *sys, Emitter *em,
                         JceParticleEventType type,
                         jce_vec3 pos, jce_vec3 vel)
{
    if (!emitter_wants_event(em, type)) return;

    if (em->event_count >= em->event_cap) {
        uint32_t newcap = em->event_cap ? em->event_cap * 2u : 32u;
        JceParticleEvent *grown = (JceParticleEvent *)sys->alloc.alloc(
            sizeof(JceParticleEvent) * newcap, sys->alloc.ctx);
        if (!grown) return; /* drop the event under memory pressure */
        if (em->events && em->event_count)
            memcpy(grown, em->events, sizeof(JceParticleEvent) * em->event_count);
        if (em->events) sys->alloc.free(em->events, sys->alloc.ctx);
        em->events    = grown;
        em->event_cap = newcap;
    }

    JceParticleEvent *ev = &em->events[em->event_count++];
    ev->type     = type;
    ev->position = pos;
    ev->velocity = vel;
}

/* ── Spawn a single particle ───────────────────────────────────────── */

static void spawn_particle(JceParticleSystem *sys, Emitter *em, uint32_t *rng)
{
    if (em->alive_count >= em->desc.max_particles) return;

    Particle *p = &em->pool[em->alive_count];
    p->position = em->origin;

    p->velocity.x = randf_range(rng, em->desc.velocity_min.x, em->desc.velocity_max.x);
    p->velocity.y = randf_range(rng, em->desc.velocity_min.y, em->desc.velocity_max.y);
    p->velocity.z = randf_range(rng, em->desc.velocity_min.z, em->desc.velocity_max.z);

    p->lifetime = randf_range(rng,
                              em->desc.lifetime_min > 0 ? em->desc.lifetime_min : 1.0f,
                              em->desc.lifetime_max > 0 ? em->desc.lifetime_max : 2.0f);
    p->age      = 0.0f;
    p->size     = em->desc.size_start > 0 ? em->desc.size_start : 0.1f;
    p->color    = em->desc.color_start;
    p->collided = false;

    /* Flipbook starts on frame 0 at birth (age 0). */
    compute_flipbook_uv(&em->desc, 0.0f, p->lifetime,
                        &p->uv_offset, &p->uv_scale, &p->frame);

    em->alive_count++;

    record_event(sys, em, JCE_PARTICLE_EVENT_BIRTH, p->position, p->velocity);
}

/* Spawn `count` child particles at a fixed world position into the child
 * emitter's own pool (serial drain pass only — never called concurrently).
 * The child uses its own desc for velocity / lifetime / colour. */
static void spawn_child_at(JceParticleSystem *sys, Emitter *child,
                           jce_vec3 pos, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (child->alive_count >= child->desc.max_particles) break;
        jce_vec3 saved = child->origin;
        child->origin = pos;
        spawn_particle(sys, child, &child->rng_state);
        child->origin = saved;
    }
}

void jce_particles_emitter_burst(JceParticleSystem *sys,
                                 JceEmitterHandle emitter, uint32_t count)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;
    Emitter *em = &sys->emitters[emitter.idx];
    if (!em->alive) return;

    for (uint32_t i = 0; i < count; i++) {
        spawn_particle(sys, em, &em->rng_state);
    }
}

/* ── Event sink / collision flag (FEATURE 8.2) ─────────────────────── */

void jce_particles_emitter_set_sink(JceParticleSystem *sys,
                                    JceEmitterHandle emitter,
                                    JceParticleEventFn cb, void *user_data)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;
    Emitter *em = &sys->emitters[emitter.idx];
    if (!em->alive) return;
    em->sink      = cb;
    em->sink_user = user_data;
}

void jce_particles_emitter_flag_collision(JceParticleSystem *sys,
                                          JceEmitterHandle emitter,
                                          uint32_t particle_idx)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;
    Emitter *em = &sys->emitters[emitter.idx];
    if (!em->alive || particle_idx >= em->alive_count) return;
    em->pool[particle_idx].collided = true;
}

/* ── Per-frame update ──────────────────────────────────────────────── */

static void update_emitter(JceParticleSystem *sys, Emitter *em,
                           float dt, uint32_t *rng)
{
    if (!em->alive) return;

    /* Spawn new particles. */
    if (em->emitting && em->desc.emit_rate > 0.0f) {
        em->emit_accumulator += em->desc.emit_rate * dt;
        while (em->emit_accumulator >= 1.0f) {
            spawn_particle(sys, em, rng);
            em->emit_accumulator -= 1.0f;
        }
    }

    /* Update alive particles. */
    const JceParticleEmitterDesc *d = &em->desc;
    uint32_t i = 0;
    while (i < em->alive_count) {
        Particle *p = &em->pool[i];

        /* Collision flag raised externally → kill now, fire COLLISION (not
         * DEATH) and run the on-death sub-emitter spawn. */
        if (p->collided) {
            record_event(sys, em, JCE_PARTICLE_EVENT_COLLISION,
                         p->position, p->velocity);
            em->alive_count--;
            if (i < em->alive_count)
                *p = em->pool[em->alive_count];
            continue; /* re-check index i */
        }

        p->age += dt;

        if (p->age >= p->lifetime) {
            /* Natural death: fire DEATH at the particle's final state. */
            record_event(sys, em, JCE_PARTICLE_EVENT_DEATH,
                         p->position, p->velocity);
            /* Swap-and-pop: replace with last alive. */
            em->alive_count--;
            if (i < em->alive_count)
                *p = em->pool[em->alive_count];
            continue; /* re-check index i */
        }

        float t = p->age / p->lifetime;  /* normalised age [0,1] */

        /* Velocity += gravity. */
        p->velocity = jce_v3_add(p->velocity, jce_v3_scale(d->gravity, dt));

        /* Position += velocity, scaled by the velocity-over-age curve.  With
         * the defaults (LINEAR curve, scale 1→1) vscale == 1 at every t, so
         * this advance is bit-identical to the legacy path. */
        float vscale = d->velocity_scale_start +
                       (d->velocity_scale_end - d->velocity_scale_start) *
                           jce_ease(d->velocity_curve, t);
        p->position = jce_v3_add(p->position,
                                 jce_v3_scale(p->velocity, dt * vscale));

        /* Interpolate size along its lifetime curve (LINEAR == legacy). */
        float s0 = d->size_start > 0 ? d->size_start : 0.1f;
        float s1 = d->size_end;
        float ts = jce_ease(d->size_curve, t);
        p->size  = s0 + (s1 - s0) * ts;

        /* Interpolate color along its lifetime curve (LINEAR == legacy). */
        float tc = jce_ease(d->color_curve, t);
        p->color.x = d->color_start.x + (d->color_end.x - d->color_start.x) * tc;
        p->color.y = d->color_start.y + (d->color_end.y - d->color_start.y) * tc;
        p->color.z = d->color_start.z + (d->color_end.z - d->color_start.z) * tc;
        p->color.w = d->color_start.w + (d->color_end.w - d->color_start.w) * tc;

        /* Advance the flipbook cell for this step (no-op when disabled). */
        compute_flipbook_uv(d, p->age, p->lifetime,
                            &p->uv_offset, &p->uv_scale, &p->frame);

        i++;
    }
}

/* Parallel emitter update.  Each emitter owns its pool + RNG + event buffer,
 * so emitters are independent and safe to step concurrently (disjoint writes).
 * Sub-emitter spawns and sink callbacks are DEFERRED to a serial drain pass so
 * the parallel step never writes another emitter's pool or runs user code. */
typedef struct { JceParticleSystem *sys; Emitter *emitters; float dt; } PUpdateCtx;

static void particles_update_range(int begin, int end, void *user)
{
    PUpdateCtx *c = (PUpdateCtx *)user;
    for (int i = begin; i < end; i++)
        update_emitter(c->sys, &c->emitters[i], c->dt, &c->emitters[i].rng_state);
}

/* Serial post-pass: drain each emitter's deferred events.  Invokes the
 * registered sink and applies sub-emitter spawns into the (sibling) child
 * pool.  Runs single-threaded so cross-emitter writes and user callbacks are
 * race-free and deterministic.  Child spawns are bounded by the child pool
 * capacity and the build-time depth cap, so a single step cannot storm. */
static void particles_drain_events(JceParticleSystem *sys)
{
    for (uint32_t i = 0; i < MAX_EMITTERS; i++) {
        Emitter *em = &sys->emitters[i];
        if (!em->alive || em->event_count == 0) continue;

        Emitter *child = NULL;
        if (jce_emitter_valid(em->sub_handle) &&
            em->sub_handle.idx < MAX_EMITTERS &&
            sys->emitters[em->sub_handle.idx].alive)
            child = &sys->emitters[em->sub_handle.idx];

        for (uint32_t e = 0; e < em->event_count; e++) {
            const JceParticleEvent *ev = &em->events[e];

            if (em->sink)
                em->sink(ev, em->sink_user);

            if (child) {
                uint32_t n = 0;
                if (ev->type == JCE_PARTICLE_EVENT_BIRTH)
                    n = em->desc.sub_spawn_on_birth;
                else /* DEATH or COLLISION */
                    n = em->desc.sub_spawn_on_death;
                if (n) spawn_child_at(sys, child, ev->position, n);
            }
        }

        em->event_count = 0; /* reset; capacity is retained for reuse */
    }
}

void jce_particles_update(JceParticleSystem *sys, float dt)
{
    if (!sys) return;
    JCE_PROFILE_ZONE_N("Particles::Update");
    PUpdateCtx ctx = { sys, sys->emitters, dt };
    JceJobSystem *jobs = jce_jobs_default();
    if (jobs)
        jce_jobs_parallel_for(jobs, MAX_EMITTERS, 0, particles_update_range, &ctx);
    else
        particles_update_range(0, MAX_EMITTERS, &ctx);

    /* Serial: deliver events + apply sub-emitter spawns (cross-emitter). */
    particles_drain_events(sys);
    JCE_PROFILE_ZONE_END;
}

/* ── Alive count ───────────────────────────────────────────────────── */

uint32_t jce_particles_alive_count(const JceParticleSystem *sys)
{
    if (!sys) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < MAX_EMITTERS; i++) {
        if (sys->emitters[i].alive)
            total += sys->emitters[i].alive_count;
    }
    return total;
}

/* ── Read-back ─────────────────────────────────────────────────────── */

uint32_t jce_particles_emitter_alive_count(const JceParticleSystem *sys,
                                           JceEmitterHandle emitter)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS)
        return 0;
    const Emitter *em = &sys->emitters[emitter.idx];
    return em->alive ? em->alive_count : 0;
}

bool jce_particles_emitter_is_alive(const JceParticleSystem *sys,
                                    JceEmitterHandle emitter)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS)
        return false;
    return sys->emitters[emitter.idx].alive;
}

void jce_particles_emitter_for_each(const JceParticleSystem *sys,
                                    JceEmitterHandle emitter,
                                    JceParticleVisitFn cb, void *user_data)
{
    if (!sys || !cb || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS)
        return;
    const Emitter *em = &sys->emitters[emitter.idx];
    if (!em->alive) return;
    for (uint32_t i = 0; i < em->alive_count; i++) {
        const Particle *p = &em->pool[i];
        JceParticleView v;
        v.position  = p->position;
        v.color     = p->color;
        v.size      = p->size;
        v.uv_offset = p->uv_offset;
        v.uv_scale  = p->uv_scale;
        v.frame     = p->frame;
        cb(&v, user_data);
    }
}

/* ── Asset I/O ─────────────────────────────────────────────────────── */

void jce_particles_desc_default(JceParticleEmitterDesc *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->max_particles = 1024;
    out->emit_rate     = 50.0f;
    out->lifetime_min  = 1.0f;
    out->lifetime_max  = 2.0f;
    out->velocity_min  = jce_v3(-0.5f, 1.0f, -0.5f);
    out->velocity_max  = jce_v3( 0.5f, 2.0f,  0.5f);
    out->gravity       = jce_v3( 0.0f, -1.0f, 0.0f);
    out->size_start    = 0.1f;
    out->size_end      = 0.0f;
    out->color_start   = jce_v4(1.0f, 1.0f, 1.0f, 1.0f);
    out->color_end     = jce_v4(1.0f, 1.0f, 1.0f, 0.0f);
    out->texture       = JCE_INVALID_TEXTURE;
    out->world_space   = false;

    /* FEATURE 8.3 — curves default to LINEAR (== memset 0) and velocity
     * scale to a flat 1.0, so the default emitter is byte-identical to the
     * legacy pure-linear sim.  Flipbook is disabled (rows/cols = 0). */
    out->size_curve           = JCE_EASE_LINEAR;
    out->color_curve          = JCE_EASE_LINEAR;
    out->velocity_curve       = JCE_EASE_LINEAR;
    out->velocity_scale_start = 1.0f;
    out->velocity_scale_end   = 1.0f;
    out->flipbook_rows        = 0u;
    out->flipbook_cols        = 0u;
    out->flipbook_fps         = 0.0f;
    out->flipbook_loop        = false;
}

/* Resolve a JSON ease field: accept a stable ease name (jce_ease_name) or a
 * raw enum ordinal; fall back to `def` when the key is absent / unrecognised. */
static JceEaseType resolve_ease(const JceJson *root, const char *key,
                                JceEaseType def)
{
    const char *s = jce_json_get_string(root, key, NULL);
    if (s && s[0]) {
        for (int t = 0; t < JCE_EASE_COUNT; t++) {
            const char *name = jce_ease_name((JceEaseType)t);
            if (name && strcmp(name, s) == 0) return (JceEaseType)t;
        }
        return def; /* string present but not a known name → keep default */
    }
    int ord = jce_json_get_int(root, key, -1);
    if (ord >= 0 && ord < JCE_EASE_COUNT) return (JceEaseType)ord;
    return def;
}

bool jce_particles_desc_load_json(const char *path, JceParticleEmitterDesc *out,
                                  char *texture_out, int texture_cap)
{
    if (!out) return false;
    jce_particles_desc_default(out);
    if (texture_out && texture_cap > 0) texture_out[0] = '\0';
    if (!path || !path[0]) return false;

    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN(LOG_TAG, "particle asset not loadable: %s", path);
        return false;
    }

    out->max_particles = (uint32_t)jce_json_get_int(root, "maxParticles",
                                                    (int)out->max_particles);
    out->emit_rate     = (float)jce_json_get_number(root, "emitRate",    out->emit_rate);
    out->emit_burst    = (float)jce_json_get_number(root, "emitBurst",   out->emit_burst);
    out->lifetime_min  = (float)jce_json_get_number(root, "lifetimeMin", out->lifetime_min);
    out->lifetime_max  = (float)jce_json_get_number(root, "lifetimeMax", out->lifetime_max);
    out->size_start    = (float)jce_json_get_number(root, "sizeStart",   out->size_start);
    out->size_end      = (float)jce_json_get_number(root, "sizeEnd",     out->size_end);
    out->world_space   = jce_json_get_bool(root, "worldSpace", out->world_space);
    jce_json_get_floats(root, "velocityMin", &out->velocity_min.x, 3, &out->velocity_min.x);
    jce_json_get_floats(root, "velocityMax", &out->velocity_max.x, 3, &out->velocity_max.x);
    jce_json_get_floats(root, "gravity",     &out->gravity.x,      3, &out->gravity.x);
    jce_json_get_floats(root, "colorStart",  &out->color_start.x,  4, &out->color_start.x);
    jce_json_get_floats(root, "colorEnd",    &out->color_end.x,    4, &out->color_end.x);

    /* FEATURE 8.3 — per-property lifetime curves.  Accept a stable ease name
     * (e.g. "quad_out") or a raw enum ordinal; absent keys keep LINEAR. */
    out->size_curve     = resolve_ease(root, "sizeCurve",     out->size_curve);
    out->color_curve    = resolve_ease(root, "colorCurve",    out->color_curve);
    out->velocity_curve = resolve_ease(root, "velocityCurve", out->velocity_curve);
    out->velocity_scale_start =
        (float)jce_json_get_number(root, "velocityScaleStart", out->velocity_scale_start);
    out->velocity_scale_end =
        (float)jce_json_get_number(root, "velocityScaleEnd",   out->velocity_scale_end);

    /* FEATURE 8.3 — flipbook / texture-sheet animation. */
    out->flipbook_rows = (uint32_t)jce_json_get_int(root, "flipbookRows", (int)out->flipbook_rows);
    out->flipbook_cols = (uint32_t)jce_json_get_int(root, "flipbookCols", (int)out->flipbook_cols);
    out->flipbook_fps  = (float)jce_json_get_number(root, "flipbookFps",  out->flipbook_fps);
    out->flipbook_loop = jce_json_get_bool(root, "flipbookLoop", out->flipbook_loop);

    /* FEATURE 8.2 — sub-emitter authoring.  A nested "subEmitter" object holds
     * a full child emitter desc; the parent carries the per-event spawn counts.
     * The child desc is heap-allocated and OWNED by `out` — the caller must
     * release it with jce_particles_desc_free once the desc has been consumed
     * (jce_particles_emitter_add deep-copies the child synchronously, so the
     * pointer only needs to outlive that single add call).  Absent object =>
     * no sub-emitter (out->sub_emitter stays NULL, byte-identical to legacy). */
    JceJson *child = jce_json_get(root, "subEmitter");
    if (child && jce_json_is_object(child)) {
        JceParticleEmitterDesc *kid =
            (JceParticleEmitterDesc *)JCE_MALLOC(sizeof(*kid));
        if (kid) {
            jce_particles_desc_default(kid);
            kid->max_particles = (uint32_t)jce_json_get_int(child, "maxParticles", (int)kid->max_particles);
            kid->emit_rate     = (float)jce_json_get_number(child, "emitRate",    kid->emit_rate);
            kid->lifetime_min  = (float)jce_json_get_number(child, "lifetimeMin", kid->lifetime_min);
            kid->lifetime_max  = (float)jce_json_get_number(child, "lifetimeMax", kid->lifetime_max);
            kid->size_start    = (float)jce_json_get_number(child, "sizeStart",   kid->size_start);
            kid->size_end      = (float)jce_json_get_number(child, "sizeEnd",     kid->size_end);
            kid->world_space   = jce_json_get_bool(child, "worldSpace", kid->world_space);
            jce_json_get_floats(child, "velocityMin", &kid->velocity_min.x, 3, &kid->velocity_min.x);
            jce_json_get_floats(child, "velocityMax", &kid->velocity_max.x, 3, &kid->velocity_max.x);
            jce_json_get_floats(child, "gravity",     &kid->gravity.x,      3, &kid->gravity.x);
            jce_json_get_floats(child, "colorStart",  &kid->color_start.x,  4, &kid->color_start.x);
            jce_json_get_floats(child, "colorEnd",    &kid->color_end.x,    4, &kid->color_end.x);
            kid->size_curve     = resolve_ease(child, "sizeCurve",     kid->size_curve);
            kid->color_curve    = resolve_ease(child, "colorCurve",    kid->color_curve);
            kid->velocity_curve = resolve_ease(child, "velocityCurve", kid->velocity_curve);
            kid->velocity_scale_start = (float)jce_json_get_number(child, "velocityScaleStart", kid->velocity_scale_start);
            kid->velocity_scale_end   = (float)jce_json_get_number(child, "velocityScaleEnd",   kid->velocity_scale_end);
            kid->flipbook_rows = (uint32_t)jce_json_get_int(child, "flipbookRows", (int)kid->flipbook_rows);
            kid->flipbook_cols = (uint32_t)jce_json_get_int(child, "flipbookCols", (int)kid->flipbook_cols);
            kid->flipbook_fps  = (float)jce_json_get_number(child, "flipbookFps",  kid->flipbook_fps);
            kid->flipbook_loop = jce_json_get_bool(child, "flipbookLoop", kid->flipbook_loop);
            if (kid->lifetime_max < kid->lifetime_min)
                kid->lifetime_max = kid->lifetime_min;
            out->sub_emitter       = kid;
            out->sub_spawn_on_birth = (uint32_t)jce_json_get_int(root, "subSpawnOnBirth", 0);
            out->sub_spawn_on_death = (uint32_t)jce_json_get_int(root, "subSpawnOnDeath", 0);
        }
    }

    if (texture_out && texture_cap > 0) {
        const char *tex = jce_json_get_string(root, "texture", "");
        if (tex && tex[0]) {
            int n = (int)strlen(tex);
            if (n >= texture_cap) n = texture_cap - 1;
            memcpy(texture_out, tex, (size_t)n);
            texture_out[n] = '\0';
        }
    }

    if (out->lifetime_max < out->lifetime_min)
        out->lifetime_max = out->lifetime_min;

    jce_json_free(root);
    return true;
}

void jce_particles_desc_free(JceParticleEmitterDesc *out)
{
    if (!out) return;
    /* The loader allocates exactly one level of child desc (it parses a single
     * nested "subEmitter" object, never recursing), so a single free matches. */
    if (out->sub_emitter) {
        JCE_FREE((void *)out->sub_emitter);
        out->sub_emitter = NULL;
    }
    out->sub_spawn_on_birth = 0u;
    out->sub_spawn_on_death = 0u;
}
