/* test_jce_particles_subemitter.c
 *
 * Validation for FEATURE 8.2 — particle events + sub-emitter chains in the
 * CPU particle simulation (jce_particles.c).  These tests drive the REAL
 * jce_particles_update path (spawn → age → death → deferred event drain →
 * sub-emitter spawn), never a mock, and lock in:
 *
 *   - a DEATH event fires at the dead particle's last position;
 *   - a wired sub-emitter spawns the configured child-particle count at that
 *     same position;
 *   - the spawn-storm caps hold (depth cap + child pool capacity);
 *   - an emitter with no sub-emitter / no sink is unaffected (no events
 *     observed, count behaves exactly as the legacy sim).
 *   - the externally-raised collision flag kills a particle, fires COLLISION
 *     (not DEATH), and drives the on-death sub-emitter spawn.
 */

#include <jce/renderer/jce_particles.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>

#include "unity.h"

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Shared event-capture sink ─────────────────────────────────────── */

typedef struct {
    int      births;
    int      deaths;
    int      collisions;
    jce_vec3 last_death_pos;
    jce_vec3 last_death_vel;
    jce_vec3 last_collision_pos;
} EventCapture;

static void capture_sink(const JceParticleEvent *ev, void *user)
{
    EventCapture *cap = (EventCapture *)user;
    switch (ev->type) {
        case JCE_PARTICLE_EVENT_BIRTH:     cap->births++;     break;
        case JCE_PARTICLE_EVENT_DEATH:
            cap->deaths++;
            cap->last_death_pos = ev->position;
            cap->last_death_vel = ev->velocity;
            break;
        case JCE_PARTICLE_EVENT_COLLISION:
            cap->collisions++;
            cap->last_collision_pos = ev->position;
            break;
    }
}

/* ── Helpers ───────────────────────────────────────────────────────── */

/* A parent emitter that emits NOTHING on its own (emit_rate=0); particles
 * arrive only via explicit bursts so the test fully controls spawn/death
 * timing and positions. */
static void parent_desc(JceParticleEmitterDesc *d)
{
    jce_particles_desc_default(d);
    d->max_particles = 64;
    d->emit_rate     = 0.0f;          /* burst-only, deterministic */
    d->lifetime_min  = 1.0f;
    d->lifetime_max  = 1.0f;          /* exact lifetime → deterministic death */
    d->velocity_min  = jce_v3(0, 0, 0);
    d->velocity_max  = jce_v3(0, 0, 0);
    d->gravity       = jce_v3(0, 0, 0); /* particle stays at spawn position */
    d->size_start    = 0.1f;
    d->size_end      = 0.1f;
    d->sub_emitter   = NULL;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

/* No sub-emitter, no sink: legacy behaviour, byte-identical death loop. */
static void test_no_subemitter_unaffected(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    JceParticleEmitterDesc d;
    parent_desc(&d);
    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));

    jce_particles_emitter_set_position(sys, h, jce_v3(5, 6, 7));
    jce_particles_emitter_burst(sys, h, 3);
    TEST_ASSERT_EQUAL_UINT32(3u, jce_particles_emitter_alive_count(sys, h));

    /* Step well past the lifetime: all particles must die. */
    for (int i = 0; i < 5; i++) jce_particles_update(sys, 0.5f);

    TEST_ASSERT_EQUAL_UINT32(0u, jce_particles_emitter_alive_count(sys, h));
    /* No emitter other than the parent exists → total alive is 0. */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_particles_alive_count(sys));

    jce_particles_destroy(sys);
}

/* DEATH event fires at the right position with a sink registered. */
static void test_death_event_position(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    JceParticleEmitterDesc d;
    parent_desc(&d);
    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));

    EventCapture cap;
    memset(&cap, 0, sizeof(cap));
    jce_particles_emitter_set_sink(sys, h, capture_sink, &cap);

    const jce_vec3 spawn = jce_v3(2.0f, 3.0f, -4.0f);
    jce_particles_emitter_set_position(sys, h, spawn);
    jce_particles_emitter_burst(sys, h, 1);
    /* Burst spawns synchronously → birth recorded but not yet drained;
     * the next update drains it. */

    /* Advance until the particle dies (lifetime = 1.0). */
    jce_particles_update(sys, 0.4f); /* age 0.4 — alive, drains the birth */
    TEST_ASSERT_EQUAL_INT(1, cap.births);
    TEST_ASSERT_EQUAL_INT(0, cap.deaths);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_particles_emitter_alive_count(sys, h));

    jce_particles_update(sys, 0.4f); /* age 0.8 — still alive */
    TEST_ASSERT_EQUAL_INT(0, cap.deaths);

    jce_particles_update(sys, 0.4f); /* age 1.2 — dies this step */
    TEST_ASSERT_EQUAL_INT(1, cap.deaths);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_particles_emitter_alive_count(sys, h));

    /* Gravity & velocity are zero → death position == spawn position. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.x, cap.last_death_pos.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.y, cap.last_death_pos.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.z, cap.last_death_pos.z);

    jce_particles_destroy(sys);
}

/* Sub-emitter spawns the configured child count at the parent's death pos. */
static void test_subemitter_spawn_on_death(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    /* Child emitter: never free-runs (set non-emitting by the engine), long
     * lifetime so spawned children persist for the assertion. */
    static JceParticleEmitterDesc child;
    jce_particles_desc_default(&child);
    child.max_particles = 64;
    child.emit_rate     = 0.0f;
    child.lifetime_min  = 100.0f;
    child.lifetime_max  = 100.0f;
    child.velocity_min  = jce_v3(0, 0, 0);
    child.velocity_max  = jce_v3(0, 0, 0);
    child.gravity       = jce_v3(0, 0, 0);
    child.sub_emitter   = NULL;

    JceParticleEmitterDesc d;
    parent_desc(&d);
    d.sub_emitter        = &child;
    d.sub_spawn_on_death = 5;
    d.sub_spawn_on_birth = 0;

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));

    /* Capture the death event too, to cross-check the spawn position. */
    EventCapture cap;
    memset(&cap, 0, sizeof(cap));
    jce_particles_emitter_set_sink(sys, h, capture_sink, &cap);

    const jce_vec3 spawn = jce_v3(10.0f, -2.0f, 3.0f);
    jce_particles_emitter_set_position(sys, h, spawn);
    jce_particles_emitter_burst(sys, h, 1);

    /* Before any death: only the 1 parent particle is alive system-wide. */
    jce_particles_update(sys, 0.4f);
    TEST_ASSERT_EQUAL_UINT32(1u, jce_particles_alive_count(sys));
    TEST_ASSERT_EQUAL_INT(0, cap.deaths);

    /* Drive the parent particle to death (total lifetime 1.0). */
    jce_particles_update(sys, 0.4f); /* 0.8 */
    jce_particles_update(sys, 0.4f); /* 1.2 → dies, spawns 5 children */

    TEST_ASSERT_EQUAL_INT(1, cap.deaths);

    /* Parent is gone; the child emitter now holds exactly 5 particles. */
    uint32_t total = jce_particles_alive_count(sys);
    TEST_ASSERT_EQUAL_UINT32(5u, total);

    /* Locate the child emitter (the only other alive emitter) and confirm
     * every child sits at the parent's death position. */
    bool found_child = false;
    for (uint32_t idx = 0; idx < 256u; idx++) {
        JceEmitterHandle eh = { idx };
        if (idx == h.idx) continue;
        if (!jce_particles_emitter_is_alive(sys, eh)) continue;
        uint32_t n = jce_particles_emitter_alive_count(sys, eh);
        if (n == 0) continue;
        found_child = true;
        TEST_ASSERT_EQUAL_UINT32(5u, n);
        break;
    }
    TEST_ASSERT_TRUE(found_child);

    /* Death position recorded by the sink equals the spawn position. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.x, cap.last_death_pos.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.y, cap.last_death_pos.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.z, cap.last_death_pos.z);

    jce_particles_destroy(sys);
}

/* Verify child spawn position via the read-back visitor (real pool data). */
typedef struct { int count; jce_vec3 expect; bool all_at_expect; } PosCheck;
static void pos_visit(const JceParticleView *p, void *user)
{
    PosCheck *pc = (PosCheck *)user;
    pc->count++;
    if (fabsf(p->position.x - pc->expect.x) > 1e-4f ||
        fabsf(p->position.y - pc->expect.y) > 1e-4f ||
        fabsf(p->position.z - pc->expect.z) > 1e-4f)
        pc->all_at_expect = false;
}

static void test_subemitter_positions_readback(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    static JceParticleEmitterDesc child;
    jce_particles_desc_default(&child);
    child.max_particles = 64;
    child.emit_rate     = 0.0f;
    child.lifetime_min  = 100.0f;
    child.lifetime_max  = 100.0f;
    child.velocity_min  = jce_v3(0, 0, 0);
    child.velocity_max  = jce_v3(0, 0, 0);
    child.gravity       = jce_v3(0, 0, 0);

    JceParticleEmitterDesc d;
    parent_desc(&d);
    d.sub_emitter        = &child;
    d.sub_spawn_on_death = 3;

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));

    const jce_vec3 spawn = jce_v3(-7.0f, 8.0f, 1.5f);
    jce_particles_emitter_set_position(sys, h, spawn);
    jce_particles_emitter_burst(sys, h, 1);

    for (int i = 0; i < 4; i++) jce_particles_update(sys, 0.4f); /* parent dies */

    /* Find the child emitter and walk its real pool. */
    PosCheck pc = { 0, spawn, true };
    for (uint32_t idx = 0; idx < 256u; idx++) {
        JceEmitterHandle eh = { idx };
        if (idx == h.idx || !jce_particles_emitter_is_alive(sys, eh)) continue;
        if (jce_particles_emitter_alive_count(sys, eh) == 0) continue;
        jce_particles_emitter_for_each(sys, eh, pos_visit, &pc);
    }
    TEST_ASSERT_EQUAL_INT(3, pc.count);
    TEST_ASSERT_TRUE(pc.all_at_expect);

    jce_particles_destroy(sys);
}

/* Spawn-storm cap: a child pool smaller than the requested spawn count must
 * cap at the pool capacity, never overflow. */
static void test_spawn_storm_pool_cap(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    static JceParticleEmitterDesc child;
    jce_particles_desc_default(&child);
    child.max_particles = 4;            /* tiny child pool */
    child.emit_rate     = 0.0f;
    child.lifetime_min  = 100.0f;
    child.lifetime_max  = 100.0f;
    child.gravity       = jce_v3(0, 0, 0);

    JceParticleEmitterDesc d;
    parent_desc(&d);
    d.max_particles      = 64;
    d.sub_emitter        = &child;
    d.sub_spawn_on_death = 1000;        /* request a storm */

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));

    jce_particles_emitter_set_position(sys, h, jce_v3(0, 0, 0));
    /* Many parents dying at once would each try to spawn 1000 children. */
    jce_particles_emitter_burst(sys, h, 10);
    for (int i = 0; i < 4; i++) jce_particles_update(sys, 0.4f);

    /* Child pool can never exceed its capacity, regardless of demand. */
    uint32_t total = jce_particles_alive_count(sys);
    TEST_ASSERT_EQUAL_UINT32(4u, total);  /* parents dead, child capped at 4 */

    jce_particles_destroy(sys);
}

/* Spawn-storm cap: a recursive sub-emitter chain is truncated at the build
 * depth cap, so the number of emitters created is bounded. */
static void test_spawn_storm_depth_cap(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    /* Self-referential desc: each emitter wants a child identical to itself.
     * Without the depth cap this would create a chain up to MAX_EMITTERS. */
    static JceParticleEmitterDesc self;
    parent_desc(&self);
    self.sub_emitter        = &self;   /* points at itself */
    self.sub_spawn_on_death = 1;

    JceEmitterHandle h = jce_particles_emitter_add(sys, &self);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));

    /* Count how many emitters got created by the recursive add.  Must be
     * bounded by the depth cap (one emitter per depth level). */
    uint32_t emitters = 0;
    for (uint32_t idx = 0; idx < 256u; idx++) {
        JceEmitterHandle eh = { idx };
        if (jce_particles_emitter_is_alive(sys, eh)) emitters++;
    }
    TEST_ASSERT_TRUE(emitters >= 1u);
    TEST_ASSERT_TRUE(emitters <= JCE_PARTICLE_SUBEMITTER_MAX_DEPTH);

    jce_particles_destroy(sys);
}

/* Collision flag kills a particle, fires COLLISION (not DEATH), and runs the
 * on-death sub-emitter spawn at the collision position. */
static void test_collision_event_and_spawn(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    static JceParticleEmitterDesc child;
    jce_particles_desc_default(&child);
    child.max_particles = 64;
    child.emit_rate     = 0.0f;
    child.lifetime_min  = 100.0f;
    child.lifetime_max  = 100.0f;
    child.gravity       = jce_v3(0, 0, 0);

    JceParticleEmitterDesc d;
    parent_desc(&d);
    d.lifetime_min       = 100.0f;     /* long life → won't die naturally */
    d.lifetime_max       = 100.0f;
    d.sub_emitter        = &child;
    d.sub_spawn_on_death = 2;          /* collision drives on-death spawn */

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));

    EventCapture cap;
    memset(&cap, 0, sizeof(cap));
    jce_particles_emitter_set_sink(sys, h, capture_sink, &cap);

    const jce_vec3 spawn = jce_v3(1.0f, 1.0f, 1.0f);
    jce_particles_emitter_set_position(sys, h, spawn);
    jce_particles_emitter_burst(sys, h, 1);
    jce_particles_update(sys, 0.1f); /* particle alive, birth drained */
    TEST_ASSERT_EQUAL_UINT32(1u, jce_particles_emitter_alive_count(sys, h));

    /* Raise the collision flag on the live particle (index 0). */
    jce_particles_emitter_flag_collision(sys, h, 0);
    jce_particles_update(sys, 0.1f); /* collision processed this step */

    TEST_ASSERT_EQUAL_INT(1, cap.collisions);
    TEST_ASSERT_EQUAL_INT(0, cap.deaths);   /* collision != natural death */
    TEST_ASSERT_EQUAL_UINT32(0u, jce_particles_emitter_alive_count(sys, h));

    /* On-death sub-emitter spawn ran: 2 children at the collision position. */
    TEST_ASSERT_EQUAL_UINT32(2u, jce_particles_alive_count(sys));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.x, cap.last_collision_pos.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.y, cap.last_collision_pos.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, spawn.z, cap.last_collision_pos.z);

    jce_particles_destroy(sys);
}

/* Small particle workloads must stay on the caller.  Dispatching the fixed
 * 256-slot table to every worker costs more than this simulation and was
 * visible as eight ~10 us tasks plus an ~90 us main-thread wait per frame. */
static void test_small_workload_stays_serial_and_reports_live_slots(void)
{
    JceParticleSystem *sys;
    JceParticleEmitterDesc d;
    JceEmitterHandle handles[4];
    JceTraceCursor cursor;
    JceTraceEvent events[128];
    bool saw_active_emitters = false;
    bool saw_alive_particles = false;
    bool saw_serial_mode = false;
    bool submitted_particle_task = false;

    jce_trace_set_enabled(true);
    jce_trace_reset();

    sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);
    parent_desc(&d);
    d.lifetime_min = 100.0f;
    d.lifetime_max = 100.0f;
    for (uint32_t i = 0; i < 4u; ++i) {
        handles[i] = jce_particles_emitter_add(sys, &d);
        TEST_ASSERT_TRUE(jce_emitter_valid(handles[i]));
    }

    /* Leave holes, including the highest occupied slot.  Observability and
     * scheduling must report the two live emitters, not MAX_EMITTERS or the
     * stale high-water slot. */
    jce_particles_emitter_remove(sys, handles[1]);
    jce_particles_emitter_remove(sys, handles[3]);
    jce_particles_emitter_burst(sys, handles[0], 1u);
    jce_particles_update(sys, 0.1f);

    jce_trace_cursor_init(&cursor);
    for (;;) {
        uint32_t count =
            jce_trace_read(&cursor, events,
                           (uint32_t)(sizeof(events) / sizeof(events[0])));
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (event->type == JCE_TRACE_EVENT_COUNTER &&
                strcmp(event->name, "particles.active_emitters") == 0) {
                saw_active_emitters = event->value_u64 == 2u;
            } else if (event->type == JCE_TRACE_EVENT_COUNTER &&
                       strcmp(event->name, "particles.alive") == 0) {
                saw_alive_particles = event->value_u64 == 1u;
            } else if (event->type == JCE_TRACE_EVENT_COUNTER &&
                       strcmp(event->name, "particles.parallel") == 0) {
                saw_serial_mode = event->value_u64 == 0u;
            } else if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT &&
                       strcmp(event->name, "particles.update") == 0) {
                submitted_particle_task = true;
            }
        }
        if (count < (uint32_t)(sizeof(events) / sizeof(events[0])))
            break;
    }

    jce_particles_destroy(sys);
    jce_thread_pool_shared_shutdown();
    jce_trace_set_enabled(false);
    jce_trace_reset();

    TEST_ASSERT_TRUE(saw_active_emitters);
    TEST_ASSERT_TRUE(saw_alive_particles);
    TEST_ASSERT_TRUE(saw_serial_mode);
    TEST_ASSERT_FALSE(submitted_particle_task);
}

static void test_large_workload_uses_available_workers_without_loss(void)
{
    enum {
        EMITTERS = 4,
        PARTICLES_PER_EMITTER = 2048
    };
    JceParticleSystem *sys;
    JceParticleEmitterDesc d;
    JceThreadPool *pool;
    JceTraceCursor cursor;
    JceTraceEvent events[128];
    bool saw_parallel = false;
    bool submitted_particle_task = false;
    int workers;

    jce_trace_set_enabled(true);
    jce_trace_reset();
    pool = jce_thread_pool_shared();
    TEST_ASSERT_NOT_NULL(pool);
    workers = jce_thread_pool_worker_count(pool);

    sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);
    parent_desc(&d);
    d.max_particles = PARTICLES_PER_EMITTER;
    d.lifetime_min = 100.0f;
    d.lifetime_max = 100.0f;
    for (uint32_t i = 0; i < EMITTERS; ++i) {
        JceEmitterHandle handle = jce_particles_emitter_add(sys, &d);
        TEST_ASSERT_TRUE(jce_emitter_valid(handle));
        jce_particles_emitter_burst(sys, handle, PARTICLES_PER_EMITTER);
    }

    jce_particles_update(sys, 0.1f);
    TEST_ASSERT_EQUAL_UINT32(EMITTERS * PARTICLES_PER_EMITTER,
                             jce_particles_alive_count(sys));

    jce_trace_cursor_init(&cursor);
    for (;;) {
        uint32_t count =
            jce_trace_read(&cursor, events,
                           (uint32_t)(sizeof(events) / sizeof(events[0])));
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (event->type == JCE_TRACE_EVENT_COUNTER &&
                strcmp(event->name, "particles.parallel") == 0)
                saw_parallel = event->value_u64 != 0u;
            else if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT &&
                     strcmp(event->name, "particles.update") == 0)
                submitted_particle_task = true;
        }
        if (count < (uint32_t)(sizeof(events) / sizeof(events[0])))
            break;
    }

    jce_particles_destroy(sys);
    jce_thread_pool_shared_shutdown();
    jce_trace_set_enabled(false);
    jce_trace_reset();

    if (workers > 1) {
        TEST_ASSERT_TRUE(saw_parallel);
        TEST_ASSERT_TRUE(submitted_particle_task);
    } else {
        TEST_ASSERT_FALSE(saw_parallel);
        TEST_ASSERT_FALSE(submitted_particle_task);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_subemitter_unaffected);
    RUN_TEST(test_death_event_position);
    RUN_TEST(test_subemitter_spawn_on_death);
    RUN_TEST(test_subemitter_positions_readback);
    RUN_TEST(test_spawn_storm_pool_cap);
    RUN_TEST(test_spawn_storm_depth_cap);
    RUN_TEST(test_collision_event_and_spawn);
    RUN_TEST(test_small_workload_stays_serial_and_reports_live_slots);
    RUN_TEST(test_large_workload_uses_available_workers_without_loss);
    return UNITY_END();
}
