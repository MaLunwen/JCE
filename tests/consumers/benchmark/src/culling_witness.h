#ifndef BENCHMARK_CULLING_WITNESS_H
#define BENCHMARK_CULLING_WITNESS_H
#include <jce/api_scene.h>
#include <jce/api_render.h>

typedef struct {
    bool requested;
    bool enabled;
    JceEntity root;
    uint32_t count;
    uint32_t groups[3];
    const char *model_path;
} BenchCulling;

bool bench_culling_initialize(JceScene *scene, const JceSceneStressDesc *desc,
                              const JceSceneStressResult *result, BenchCulling *state);
void bench_culling_configure(const BenchCulling *state, JceSceneRenderConfig *config);
bool bench_culling_observe(const BenchCulling *state, JceSceneRenderer *renderer,
                           unsigned frame);
#endif
