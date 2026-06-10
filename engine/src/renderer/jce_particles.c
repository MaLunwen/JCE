/*
 * jce_particles.c  CPU-based particle system implementation.
 *
 * Each emitter owns a flat pool of particles.  Alive particles
 * are packed at the front; dead ones swap to the end (unstable sort)
 * for cache-friendly iteration.
 */

#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_jobs.h>      /* parallel emitter update */
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_particles.h>

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

            /* Distinct per-emitter seed (never 0 — xorshift32 stalls on 0)
             * so each emitter's randomness is independent → parallel-safe. */
            em->rng_state = ((uint32_t)i + 1u) * 2654435761u ^ 0x9E3779B9u;
            if (em->rng_state == 0u) em->rng_state = 0xDEADBEEFu;

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
        spawn_particle(em, &em->rng_state);
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

/* Parallel emitter update.  Each emitter owns its pool + RNG, so emitters
 * are independent and safe to step concurrently (disjoint writes). */
typedef struct { Emitter *emitters; float dt; } PUpdateCtx;

static void particles_update_range(int begin, int end, void *user)
{
    PUpdateCtx *c = (PUpdateCtx *)user;
    for (int i = begin; i < end; i++)
        update_emitter(&c->emitters[i], c->dt, &c->emitters[i].rng_state);
}

void jce_particles_update(JceParticleSystem *sys, float dt)
{
    if (!sys) return;
    JCE_PROFILE_ZONE_N("Particles::Update");
    PUpdateCtx ctx = { sys->emitters, dt };
    JceJobSystem *jobs = jce_jobs_default();
    if (jobs)
        jce_jobs_parallel_for(jobs, MAX_EMITTERS, 0, particles_update_range, &ctx);
    else
        particles_update_range(0, MAX_EMITTERS, &ctx);
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
        v.position = p->position;
        v.color    = p->color;
        v.size     = p->size;
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
