/* Controlled SDK-only contact workload, separate from free-fall rendering. */
#include "probes.h"
#include <jce/api_physics.h>
#include <math.h>
#include <string.h>

#define LOG_TAG "benchmark_probe"
#define CONTACT_WARMUP 600u

typedef struct Contacts {
    bool *touched;
    uint32_t count;
    uint64_t events;
} Contacts;

static void contact_event(const JceContactEvent *event, void *user)
{
    Contacts *contacts = user;
    if (event->type == JCE_CONTACT_END || event->is_trigger) return;
    const uint64_t tags[2] = {event->entity_a, event->entity_b};
    for (uint32_t i = 0; i < 2; ++i)
        if (tags[i] && tags[i] <= contacts->count)
            contacts->touched[tags[i] - 1u] = true;
    ++contacts->events;
}

static jce_vec3 initial_position(uint32_t index)
{
    return jce_v3((float)(index % 20u) * 1.5f,
                  0.55f + (float)(index / 400u) * 1.0f,
                  (float)((index / 20u) % 20u) * 1.5f);
}

bool bench_probe_contacts(const char *name, const BenchProbeConfig *config)
{
    if (config->count > 10000u || config->concurrency != 1u || config->depth != 1u) return false;
    const bool disabled = !strcmp(name, "physics-disabled");
    JcePhysicsWorldDesc wd = {0};
    wd.gravity = jce_v3(0, -9.81f, 0);
    wd.max_bodies = config->count + 1u;
    wd.fixed_timestep = 1.0f / 60.0f;
    wd.max_sub_steps = 1;
    wd.split_impulse = -1;
    JcePhysicsWorld *world = jce_physics_create(&wd);
    JceBodyHandle *bodies = jce_malloc((size_t)config->count * sizeof(*bodies));
    Contacts contacts = {jce_malloc((size_t)config->count * sizeof(bool)), config->count, 0};
    if (contacts.touched) memset(contacts.touched, 0, (size_t)config->count * sizeof(bool));
    bool correct = world && bodies && contacts.touched;
    if (correct) correct = jce_physics_add_contact_listener(world, contact_event, &contacts);
    JceBodyDesc desc = {0};
    desc.type = JCE_BODY_STATIC;
    desc.shape = JCE_SHAPE_BOX;
    desc.position = jce_v3(0, -0.5f, 0);
    desc.rotation = jce_q_identity();
    desc.half_extents = jce_v3(40, 0.5f, 40);
    if (correct) correct = jce_body_valid(jce_physics_body_create(world, &desc));
    desc.type = JCE_BODY_DYNAMIC;
    desc.mass = 1;
    desc.friction = 0.5f;
    desc.half_extents = jce_v3(0.45f, 0.45f, 0.45f);
    for (uint32_t i = 0; correct && i < config->count; ++i) {
        desc.position = initial_position(i);
        bodies[i] = jce_physics_body_create(world, &desc);
        correct = jce_body_valid(bodies[i]);
        if (correct) {
            jce_physics_body_set_entity(world, bodies[i], (uint64_t)i + 1u);
            if (disabled) jce_physics_body_set_active(world, bodies[i], false);
        }
    }
    LOG_INFO(LOG_TAG, "benchmark-contact-scope: warmup_steps=%u sample_steps=1 "
             "fixed_hz=60 population=%u disabled=%u solver=default mt=0 awake_control=1",
             CONTACT_WARMUP, config->count, disabled ? 1u : 0u);
    for (uint32_t i = 0; correct && i < CONTACT_WARMUP; ++i) {
        if (!disabled)
            for (uint32_t body = 0; body < config->count; ++body)
                jce_physics_body_set_active(world, bodies[body], true);
        jce_physics_step(world, wd.fixed_timestep);
    }
    for (uint32_t round = 0; correct && round < config->rounds; ++round) {
        /* Prepare a sustained solver load, rather than timing a sleeping stack.
         * Wake traversal and validation stay outside the physics-step interval. */
        if (!disabled)
            for (uint32_t body = 0; body < config->count; ++body)
                jce_physics_body_set_active(world, bodies[body], true);
        uint64_t started = jce_time_perf_counter();
        jce_physics_step(world, wd.fixed_timestep);
        uint64_t ended = jce_time_perf_counter();
        uint64_t completed = 0, touched = 0;
        correct = jce_physics_body_count(world) == config->count + 1u;
        for (uint32_t i = 0; correct && i < config->count; ++i) {
            jce_vec3 position;
            jce_physics_body_get_transform(world, bodies[i], &position, NULL);
            correct = isfinite(position.x) && isfinite(position.y) && isfinite(position.z);
            if (disabled) {
                jce_vec3 initial = initial_position(i);
                correct = correct && fabsf(position.x - initial.x) < 0.0001f &&
                          fabsf(position.y - initial.y) < 0.0001f &&
                          fabsf(position.z - initial.z) < 0.0001f;
            } else {
                correct = correct && position.y >= 0.35f &&
                          position.y <= 1.0f + (float)(config->count / 400u);
            }
            touched += contacts.touched[i] ? 1u : 0u;
            ++completed;
        }
        correct = correct && completed == config->count &&
                  (disabled ? contacts.events == 0 : touched == config->count && contacts.events > 0);
        bench_probe_sample(name, config, round, started, ended, completed, touched, correct);
        LOG_INFO(LOG_TAG, "benchmark-contact-state: round=%u bodies=%u touched=%llu events=%llu",
                 round, jce_physics_body_count(world), (unsigned long long)touched,
                 (unsigned long long)contacts.events);
    }
    if (world) jce_physics_destroy(world);
    jce_free(contacts.touched);
    jce_free(bodies);
    return correct;
}
