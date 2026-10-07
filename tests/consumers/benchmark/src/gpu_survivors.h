#ifndef BENCHMARK_GPU_SURVIVORS_H
#define BENCHMARK_GPU_SURVIVORS_H

#include <jce/api_render.h>
#include <jce/api_graphics.h>

typedef struct BenchGpuSurvivorRequest {
    JceRenderReadbackTicket ticket;
    JceGpuSceneSurvivorSnapshot snapshot;
    unsigned frame;
} BenchGpuSurvivorRequest;

typedef struct BenchGpuSurvivors {
    JceRenderReadbackQueue *queue;
    BenchGpuSurvivorRequest requests[JCE_RENDER_READBACK_QUEUE_CAPACITY];
    const char *path;
    bool written;
    bool failed;
} BenchGpuSurvivors;

bool bench_gpu_survivors_create(BenchGpuSurvivors *state, JceRenderer *renderer);
void bench_gpu_survivors_tick(BenchGpuSurvivors *state,
                              JceSceneRenderer *renderer, unsigned frame);
void bench_gpu_survivors_tick_raw(BenchGpuSurvivors *state,
                                  JceGpuScene *scene, unsigned frame);
void bench_gpu_survivors_destroy(BenchGpuSurvivors *state);

#endif
