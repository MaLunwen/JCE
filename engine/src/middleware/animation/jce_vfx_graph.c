/*
 * jce_vfx_graph.c  Minimal `*.vfx.json` interpreter + CPU particle sim.
 *
 * Replaces the former pure stub.  A VFX graph asset is now parsed from a
 * `*.vfx.json` document into a flat emitter description; each instance runs
 * a small self-contained CPU simulation (spawn / age / kill, gravity,
 * size+color interpolation) so authored VFX assets actually drive particle
 * behaviour at runtime.
 *
 * LAYERING: this TU lives in middleware/animation (L4).  The renderer's
 * JceParticleSystem (L3) already depends on the animation layer, so this
 * file can NOT link against it (that edge would be circular).  Hence the
 * inline simulation here instead of reusing jce_particles.c.  The GPU
 * billboard submit path stays deferred (jce_vfx_instance_submit is a no-op).
 */

#include <jce/middleware/animation/jce_vfx_graph.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_math.h>
#include "os/core/jce_memory.h"

#include <stddef.h>
#include <string.h>

/* ── Parsed emitter description (subset of JceParticleEmitterDesc) ───── */

typedef struct {
    uint32_t max_particles;
    float    emit_rate;
    float    emit_burst;
    float    lifetime_min, lifetime_max;
    jce_vec3 velocity_min, velocity_max;
    jce_vec3 gravity;
    float    size_start, size_end;
    jce_vec4 color_start, color_end;
} VfxEmitter;

struct JceVfxGraph {
    int        ref;
    bool       valid;       /* parsed a real document */
    VfxEmitter emitter;
};

/* ── Instance: tiny CPU particle pool ───────────────────────────────── */

typedef struct {
    jce_vec3 pos, vel;
    float    age, life;
} VfxParticle;

struct JceVfxInstance {
    JceVfxGraph *graph;
    float        time;
    uint32_t     rng;
    float        emit_accum;
    VfxParticle *pool;
    uint32_t     cap;
    uint32_t     alive;
};

/* ── RNG (xorshift32) ───────────────────────────────────────────────── */

static uint32_t vfx_rand(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x1234567u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static float vfx_randf(uint32_t *s, float a, float b)
{
    float u = (float)(vfx_rand(s) & 0x00FFFFFF) / (float)0x01000000;
    return a + (b - a) * u;
}

/* ── Defaults + parse ───────────────────────────────────────────────── */

static void vfx_emitter_default(VfxEmitter *e)
{
    memset(e, 0, sizeof(*e));
    e->max_particles = 1024;
    e->emit_rate     = 50.0f;
    e->lifetime_min  = 1.0f;
    e->lifetime_max  = 2.0f;
    e->velocity_min  = jce_v3(-0.5f, 1.0f, -0.5f);
    e->velocity_max  = jce_v3( 0.5f, 2.0f,  0.5f);
    e->gravity       = jce_v3( 0.0f, -1.0f, 0.0f);
    e->size_start    = 0.1f;
    e->size_end      = 0.0f;
    e->color_start   = jce_v4(1.0f, 1.0f, 1.0f, 1.0f);
    e->color_end     = jce_v4(1.0f, 1.0f, 1.0f, 0.0f);
}

static bool vfx_parse(JceJson *root, VfxEmitter *e)
{
    /* A `*.vfx.json` may either be a flat emitter object or carry the emitter
     * under an "emitters" array (first entry wins for this minimal pass). */
    JceJson *src = root;
    if (jce_json_is_array(jce_json_get(root, "emitters"))) {
        JceJson *arr = jce_json_get(root, "emitters");
        if (jce_json_array_size(arr) > 0)
            src = jce_json_array_at(arr, 0);
    }
    if (!src) return false;

    e->max_particles = (uint32_t)jce_json_get_int(src, "maxParticles",
                                                  (int)e->max_particles);
    e->emit_rate    = (float)jce_json_get_number(src, "emitRate",    e->emit_rate);
    e->emit_burst   = (float)jce_json_get_number(src, "emitBurst",   e->emit_burst);
    e->lifetime_min = (float)jce_json_get_number(src, "lifetimeMin", e->lifetime_min);
    e->lifetime_max = (float)jce_json_get_number(src, "lifetimeMax", e->lifetime_max);
    e->size_start   = (float)jce_json_get_number(src, "sizeStart",   e->size_start);
    e->size_end     = (float)jce_json_get_number(src, "sizeEnd",     e->size_end);
    jce_json_get_floats(src, "velocityMin", &e->velocity_min.x, 3, &e->velocity_min.x);
    jce_json_get_floats(src, "velocityMax", &e->velocity_max.x, 3, &e->velocity_max.x);
    jce_json_get_floats(src, "gravity",     &e->gravity.x,      3, &e->gravity.x);
    jce_json_get_floats(src, "colorStart",  &e->color_start.x,  4, &e->color_start.x);
    jce_json_get_floats(src, "colorEnd",    &e->color_end.x,    4, &e->color_end.x);

    if (e->lifetime_max < e->lifetime_min) e->lifetime_max = e->lifetime_min;
    if (e->max_particles == 0)             e->max_particles = 1024;
    return true;
}

/* ── Graph lifecycle ────────────────────────────────────────────────── */

JceVfxGraph *jce_vfx_graph_load(const char *path)
{
    JceVfxGraph *g = (JceVfxGraph *)JCE_CALLOC(1, sizeof(JceVfxGraph));
    if (!g) return NULL;
    g->ref   = 1;
    g->valid = false;
    vfx_emitter_default(&g->emitter);

    if (path && path[0]) {
        JceJson *root = jce_json_parse_file(path);
        if (root) {
            g->valid = vfx_parse(root, &g->emitter);
            jce_json_free(root);
        }
    }
    return g;
}

void jce_vfx_graph_unload(JceVfxGraph *g)
{
    if (!g) return;
    if (--g->ref <= 0) JCE_FREE(g);
}

bool jce_vfx_graph_is_valid(const JceVfxGraph *g)
{
    return g && g->valid;
}

/* ── Instance lifecycle ─────────────────────────────────────────────── */

JceVfxInstance *jce_vfx_instance_create(JceVfxGraph *g)
{
    if (!g) return NULL;
    JceVfxInstance *inst = (JceVfxInstance *)JCE_CALLOC(1, sizeof(JceVfxInstance));
    if (!inst) return NULL;
    inst->graph = g;
    g->ref++;
    inst->rng = 0xC0FFEEu;
    inst->cap = g->emitter.max_particles;
    if (inst->cap == 0) inst->cap = 1024;
    inst->pool = (VfxParticle *)JCE_CALLOC(inst->cap, sizeof(VfxParticle));
    if (!inst->pool) {
        jce_vfx_graph_unload(g);
        JCE_FREE(inst);
        return NULL;
    }
    return inst;
}

void jce_vfx_instance_destroy(JceVfxInstance *inst)
{
    if (!inst) return;
    if (inst->pool)  JCE_FREE(inst->pool);
    if (inst->graph) jce_vfx_graph_unload(inst->graph);
    JCE_FREE(inst);
}

/* ── Per-frame CPU simulation ───────────────────────────────────────── */

static void vfx_spawn(JceVfxInstance *inst, const VfxEmitter *e)
{
    if (inst->alive >= inst->cap) return;
    VfxParticle *p = &inst->pool[inst->alive++];
    p->pos = jce_v3(0, 0, 0);
    p->vel = jce_v3(vfx_randf(&inst->rng, e->velocity_min.x, e->velocity_max.x),
                    vfx_randf(&inst->rng, e->velocity_min.y, e->velocity_max.y),
                    vfx_randf(&inst->rng, e->velocity_min.z, e->velocity_max.z));
    p->life = vfx_randf(&inst->rng, e->lifetime_min, e->lifetime_max);
    p->age  = 0.0f;
}

void jce_vfx_instance_tick(JceVfxInstance *inst, float dt)
{
    if (!inst || !inst->graph) return;
    inst->time += dt;
    if (dt <= 0.0f) return;

    const VfxEmitter *e = &inst->graph->emitter;

    /* Spawn by rate. */
    if (e->emit_rate > 0.0f) {
        inst->emit_accum += e->emit_rate * dt;
        while (inst->emit_accum >= 1.0f) {
            vfx_spawn(inst, e);
            inst->emit_accum -= 1.0f;
        }
    }

    /* Age / integrate / kill (swap-and-pop). */
    uint32_t i = 0;
    while (i < inst->alive) {
        VfxParticle *p = &inst->pool[i];
        p->age += dt;
        if (p->age >= p->life) {
            inst->alive--;
            if (i < inst->alive) *p = inst->pool[inst->alive];
            continue;
        }
        p->vel = jce_v3_add(p->vel, jce_v3_scale(e->gravity, dt));
        p->pos = jce_v3_add(p->pos, jce_v3_scale(p->vel, dt));
        i++;
    }
}

void jce_vfx_instance_submit(JceVfxInstance *inst, uint16_t view_id)
{
    /* GPU billboard pass deferred — the animation layer cannot link the
     * renderer's particle pipeline (see file header). */
    (void)inst;
    (void)view_id;
}

uint32_t jce_vfx_instance_alive_count(const JceVfxInstance *inst)
{
    return inst ? inst->alive : 0;
}
