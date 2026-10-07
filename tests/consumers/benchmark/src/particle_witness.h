#ifndef BENCH_PARTICLE_WITNESS_H
#define BENCH_PARTICLE_WITNESS_H

#include <jce/api_scene.h>
#include <jce/api_render.h>
#include <jce/api_graphics.h>

#define BENCH_PARTICLE_MAX 64u
#define BENCH_PARTICLE_QUEUES ((BENCH_PARTICLE_MAX + JCE_RENDER_READBACK_QUEUE_CAPACITY - 1u) / JCE_RENDER_READBACK_QUEUE_CAPACITY)
typedef struct BenchParticleReadback {
    JceRenderReadbackQueue *queues[BENCH_PARTICLE_QUEUES];
    JceRenderReadbackTicket tickets[BENCH_PARTICLE_MAX];
    char names[BENCH_PARTICLE_MAX][64];
    unsigned requested, completed, stats_until_frame;
    bool stats_disabled;
    bool enabled, failed, cpu;
} BenchParticleReadback;

bool bench_particle_readback_create(BenchParticleReadback *state, JceRenderer *renderer);
void bench_particle_readback_tick(BenchParticleReadback *state, JceSceneRenderer *renderer, unsigned frame);
void bench_particle_readback_destroy(BenchParticleReadback *state);

void bench_particle_observe(JceScene *scene, unsigned frame);

#endif
