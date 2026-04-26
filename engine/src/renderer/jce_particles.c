/*
 * jce_particles.c  CPU-based particle system implementation.
 *
 * Each emitter owns a flat pool of particles.  Alive particles
 * are packed at the front; dead ones swap to the end (unstable sort)
 * for cache-friendly iteration.
 */

#include <jce/renderer/jce_particles.h>
#include <jce/os/core/jce_log.h>

#include <string.h>
#include <stdlib.h>
#include <math.h>

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
    }
    sys->alloc.free(sys, sys->alloc.ctx);
    /* LOG after free is unsafe — omitted intentionally. */
}

/* ── Emitter management ────────────────────────────────────────────── */

JceEmitterHandle jce_particles_emitter_add(JceParticleSystem *sys,
                                           const JceParticleEmitterDesc *desc)
{
    if (!sys || !desc) return JCE_EMITTER_INVALID;

    for (uint32_t i = 0; i < MAX_EMITTERS; i++) {
        if (!sys->emitters[i].alive) {
            Emitter *em = &sys->emitters[i];
            memset(em, 0, sizeof(*em));
            em->alive    = true;
            em->emitting = true;
            em->desc     = *desc;
            em->origin   = jce_v3(0, 0, 0);

            uint32_t cap = desc->max_particles ? desc->max_particles : 1024;
            em->pool = (Particle *)sys->alloc.alloc(
                sizeof(Particle) * cap, sys->alloc.ctx);
            if (!em->pool) {
                em->alive = false;
                return JCE_EMITTER_INVALID;
            }
            memset(em->pool, 0, sizeof(Particle) * cap);
            em->desc.max_particles = cap;

            sys->emitter_count++;
            return (JceEmitterHandle){ i };
        }
    }

    LOG_ERROR(LOG_TAG, "emitter limit reached (%u)", MAX_EMITTERS);
    return JCE_EMITTER_INVALID;
}

void jce_particles_emitter_remove(JceParticleSystem *sys,
                                  JceEmitterHandle emitter)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;

    Emitter *em = &sys->emitters[emitter.idx];
    if (em->alive) {
        if (em->pool)
            sys->alloc.free(em->pool, sys->alloc.ctx);
        em->pool  = NULL;
        em->alive = false;
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

/* ── Spawn a single particle ───────────────────────────────────────── */

static void spawn_particle(Emitter *em, uint32_t *rng)
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
    p->age  = 0.0f;
    p->size = em->desc.size_start > 0 ? em->desc.size_start : 0.1f;
    p->color = em->desc.color_start;

    em->alive_count++;
}

void jce_particles_emitter_burst(JceParticleSystem *sys,
                                 JceEmitterHandle emitter, uint32_t count)
{
    if (!sys || !jce_emitter_valid(emitter) || emitter.idx >= MAX_EMITTERS) return;
    Emitter *em = &sys->emitters[emitter.idx];
    if (!em->alive) return;

    for (uint32_t i = 0; i < count; i++) {
        spawn_particle(em, &sys->rng_state);
    }
}

/* ── Per-frame update ──────────────────────────────────────────────── */

static void update_emitter(Emitter *em, float dt, uint32_t *rng)
{
    if (!em->alive) return;

    /* Spawn new particles. */
    if (em->emitting && em->desc.emit_rate > 0.0f) {
        em->emit_accumulator += em->desc.emit_rate * dt;
        while (em->emit_accumulator >= 1.0f) {
            spawn_particle(em, rng);
            em->emit_accumulator -= 1.0f;
        }
    }

    /* Update alive particles. */
    const JceParticleEmitterDesc *d = &em->desc;
    uint32_t i = 0;
    while (i < em->alive_count) {
        Particle *p = &em->pool[i];
        p->age += dt;

        if (p->age >= p->lifetime) {
            /* Swap-and-pop: replace with last alive. */
            em->alive_count--;
            if (i < em->alive_count)
                *p = em->pool[em->alive_count];
            continue; /* re-check index i */
        }

        float t = p->age / p->lifetime;  /* normalised age [0,1] */

        /* Velocity += gravity. */
        p->velocity = jce_v3_add(p->velocity, jce_v3_scale(d->gravity, dt));

        /* Position += velocity. */
        p->position = jce_v3_add(p->position, jce_v3_scale(p->velocity, dt));

        /* Interpolate size. */
        float s0 = d->size_start > 0 ? d->size_start : 0.1f;
        float s1 = d->size_end;
        p->size = s0 + (s1 - s0) * t;

        /* Interpolate color. */
        p->color.x = d->color_start.x + (d->color_end.x - d->color_start.x) * t;
        p->color.y = d->color_start.y + (d->color_end.y - d->color_start.y) * t;
        p->color.z = d->color_start.z + (d->color_end.z - d->color_start.z) * t;
        p->color.w = d->color_start.w + (d->color_end.w - d->color_start.w) * t;

        i++;
    }
}

void jce_particles_update(JceParticleSystem *sys, float dt)
{
    if (!sys) return;
    for (uint32_t i = 0; i < MAX_EMITTERS; i++) {
        update_emitter(&sys->emitters[i], dt, &sys->rng_state);
    }
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
