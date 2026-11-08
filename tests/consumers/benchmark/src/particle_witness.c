/* Observe existing scene/component runtime; never implement particle rendering. */
#include "particle_witness.h"
#include <jce/api_core.h>
#include <stdlib.h>

/* Finite opt-in evidence must survive DIST without enabling ordinary chatter. */
#define PARTICLE_DIAGNOSTIC(...) \
    jce_log_write(JCE_LOG_LEVEL_INFO, "benchmark_particles", __FILE__, __LINE__, __VA_ARGS__)

typedef struct ParticlePopulation {
    unsigned components;
    unsigned loaded;
    unsigned cpu_handles;
    unsigned gpu_requested;
} ParticlePopulation;

static void observe_emitter(JceScene *scene, JceEntity entity, void *user)
{
    ParticlePopulation *population = user;
    JceParticleEmitterComponent *component = jce_scene_get_particle_emitter(scene, entity);
    if (!component) return;
    population->components++;
    population->loaded += component->loaded ? 1u : 0u;
    population->cpu_handles += component->loaded && component->emitter_handle_idx != UINT32_MAX;
    population->gpu_requested += component->gpu ? 1u : 0u;
}

void bench_particle_observe(JceScene *scene, unsigned frame)
{
    const char *enabled = getenv("JCE_BENCH_PARTICLE_OBSERVE");
    if (!enabled || enabled[0] != '1' || frame != 50) return;
    jce_log_set_level(JCE_LOG_LEVEL_INFO);
    ParticlePopulation population = {0};
    jce_scene_each_particle_emitter(scene, observe_emitter, &population);
    PARTICLE_DIAGNOSTIC(
        "benchmark-particles: frame=%u components=%u loaded=%u cpu_handles=%u gpu_requested=%u",
        frame, population.components, population.loaded,
        population.cpu_handles, population.gpu_requested);
}

/* Public typed asynchronous readback, diagnostics only. */
#include <string.h>
#include <stdio.h>

bool bench_particle_readback_create(BenchParticleReadback *state, JceRenderer *renderer)
{
    const char *enabled = getenv("JCE_BENCH_PARTICLE_STATS");
    if (!enabled) enabled = getenv("JCE_BENCH_PARTICLE_GPU_STATS");
    if (!enabled || enabled[0] != '1') return true;
    JceCvar *cv = jce_cvar_find("r.particles.debug_stats");
    if (!cv || !renderer) return false;
    jce_cvar_set_bool(cv, true);
    state->enabled = true;
    jce_log_set_level(JCE_LOG_LEVEL_INFO);
    PARTICLE_DIAGNOSTIC("benchmark-particle-backend: reported=%s",
        jce_renderer_running_backend_name());
    const char *until = getenv("JCE_BENCH_PARTICLE_STATS_UNTIL_FRAME");
    if (until && until[0]) {
        char *end = NULL;
        unsigned long value = strtoul(until, &end, 10);
        if (!end || *end || (value != 0 && value < 50) || value >= 120) return false;
        state->stats_until_frame = (unsigned)value;
    }
    const char *force = getenv("JCE_GPU_PARTICLES_FORCE");
    state->cpu = !force || force[0] == '0';
    for (unsigned i = 0; i < BENCH_PARTICLE_QUEUES; ++i) {
        state->queues[i] = jce_render_readback_create(renderer);
        if (!state->queues[i]) { state->failed = true; return false; }
    }
    return true;
}

void bench_particle_readback_tick(BenchParticleReadback *state, JceSceneRenderer *renderer, unsigned frame)
{
    if (!state->enabled || state->failed) return;
    for (unsigned i = 0; i < state->requested; ++i) {
        if (!state->tickets[i]) continue;
        JceRenderReadbackQueue *queue = state->queues[i / JCE_RENDER_READBACK_QUEUE_CAPACITY];
        JceRenderReadbackState ready = jce_render_readback_poll(queue, state->tickets[i]);
        if (ready == JCE_RENDER_READBACK_PENDING) continue;
        JceRenderReadbackInfo info;
        unsigned char rgba[4];
        if (ready != JCE_RENDER_READBACK_READY ||
            !jce_render_readback_get_info(queue, state->tickets[i], &info) ||
            info.format != JCE_RENDER_FORMAT_RGBA8 || info.width != 1 || info.height != 1 ||
            info.byte_size != 4 || !jce_render_readback_copy(queue, state->tickets[i], rgba, 4) ||
            rgba[3] != 255) goto failed;
        unsigned alive = (unsigned)rgba[0] | ((unsigned)rgba[1] << 8) | ((unsigned)rgba[2] << 16);
        PARTICLE_DIAGNOSTIC(
            "benchmark-particle-alive: source_frame=50 completed_frame=%u ready_engine_frame=%u index=%u name=%s rgba=%02x%02x%02x%02x alive=%u",
            frame, info.ready_frame, i, state->names[i], rgba[0], rgba[1], rgba[2], rgba[3], alive);
        jce_render_readback_release(queue, state->tickets[i]);
        state->tickets[i] = 0;
        ++state->completed;
    }
    if (state->stats_until_frame && !state->stats_disabled &&
        frame >= state->stats_until_frame && state->requested &&
        state->completed == state->requested) {
        JceCvar *cv = jce_cvar_find("r.particles.debug_stats");
        if (!cv) goto failed;
        jce_cvar_set_bool(cv, false);
        state->stats_disabled = true;
        PARTICLE_DIAGNOSTIC( "benchmark-particle-stats-disabled: frame=%u requested=%u completed=%u",
            frame, state->requested, state->completed);
    }
    if (frame != 50) return;
    int targets = jce_scene_renderer_debug_target_count(renderer);
    for (int t = 0; t < targets; ++t) {
        JceRenderTargetInfo target;
        if (!jce_scene_renderer_debug_target_get(renderer, t, &target)) goto failed;
        if (state->cpu ? (strcmp(target.name, "particles.cpu_alive_total") != 0 &&
                          strcmp(target.name, "particles.cpu_drawn_total") != 0)
                       : strncmp(target.name, "particles.gpu_alive_", 20) != 0) continue;
        unsigned i = state->requested;
        if (i >= BENCH_PARTICLE_MAX || target.width != 1 || target.height != 1) goto failed;
        JceRenderReadbackDesc desc = jce_render_readback_desc_default();
        desc.source = target.texture;
        desc.width = desc.height = 1;
        desc.format = JCE_RENDER_FORMAT_RGBA8;
        desc.view_id = 251;
        snprintf(state->names[i], sizeof state->names[i], "%s", target.name);
        if (!jce_render_readback_submit(state->queues[i / JCE_RENDER_READBACK_QUEUE_CAPACITY],
                &desc, &state->tickets[i])) goto failed;
        ++state->requested;
    }
    PARTICLE_DIAGNOSTIC( "benchmark-particle-requests: source_frame=50 count=%u", state->requested);
    if (!state->requested || (state->cpu && state->requested != 2)) goto failed;
    return;
failed:
    state->failed = true;
    LOG_ERROR("benchmark_particles", "Particle typed readback failed frame=%u", frame);
}

void bench_particle_readback_destroy(BenchParticleReadback *state)
{
    if (state->enabled && (state->failed || !state->requested || state->requested != state->completed))
        LOG_ERROR("benchmark_particles", "Particle requests incomplete requested=%u completed=%u",
            state->requested, state->completed);
    for (unsigned i = 0; i < BENCH_PARTICLE_QUEUES; ++i)
        jce_render_readback_destroy(state->queues[i]);
    memset(state, 0, sizeof *state);
}
