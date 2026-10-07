#ifndef BENCH_LOD_WITNESS_H
#define BENCH_LOD_WITNESS_H

#include <jce/api_graphics.h>
#include <jce/api_scene.h>
#include <jce/middleware/scene/jce_lod.h>

typedef struct BenchLod {
    JceMesh *meshes[3];
    JceLodGroup group;
    bool requested;
    bool enabled;
} BenchLod;

bool bench_lod_create(BenchLod *state, JceSceneRendererCallbacks *callbacks);
void bench_lod_bind(BenchLod *state, JceSceneRenderer *renderer);
void bench_lod_destroy(BenchLod *state);
bool bench_lod_update(const BenchLod *state, JceCamera *camera, unsigned frame);
void bench_lod_configure(const BenchLod *state, JceSceneRenderConfig *config);
bool bench_lod_observe(const BenchLod *state, JceSceneRenderer *renderer,
                       JceCamera *camera, unsigned frame);

#endif
