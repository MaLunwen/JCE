/* Hardware-query fixture owned by the SDK benchmark consumer. */
#ifndef BENCH_OCCLUSION_WITNESS_H
#define BENCH_OCCLUSION_WITNESS_H
#include <jce/api_app.h>
#include <jce/api_scene.h>
#include <jce/api_render.h>

typedef struct BenchOcclusion {
    JceOcclusionCuller *culler;
    JceEntity root;
    JceEntity wall;
    uint32_t count;
    uint32_t positioned;
    const char *model_path;
    bool requested;
    bool enabled;
    bool reveal;
} BenchOcclusion;

bool bench_occlusion_initialize(JceScene *scene, const JceSceneStressDesc *desc,
    const JceSceneStressResult *result, const JceServices *services, BenchOcclusion *state);
bool bench_occlusion_update(JceScene *scene, BenchOcclusion *state, unsigned frame);
void bench_occlusion_configure(const BenchOcclusion *state, JceSceneRenderConfig *config);
bool bench_occlusion_observe(const BenchOcclusion *state, unsigned frame);
void bench_occlusion_destroy(BenchOcclusion *state);
#endif
