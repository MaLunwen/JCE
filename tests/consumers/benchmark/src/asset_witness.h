#ifndef BENCHMARK_ASSET_WITNESS_H
#define BENCHMARK_ASSET_WITNESS_H

#include <jce/api_scene.h>
#include <jce/api_render.h>

typedef struct {
    const char *path;
    uint64_t started_ms;
    bool ready;
} BenchAssetReadiness;

bool bench_asset_readiness_initialize(BenchAssetReadiness *state);
bool bench_asset_readiness_update(BenchAssetReadiness *state,
                                 JceSceneRenderer *renderer, unsigned frame);
bool bench_asset_witness(JceScene *scene, JceSceneRenderer *renderer, unsigned frame);
bool bench_asset_start(JceScene *scene, JceSceneRenderer *renderer, unsigned frame);
bool bench_asset_capture_configure(void);

#endif
