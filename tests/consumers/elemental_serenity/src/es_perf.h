/*
 * es_perf.h — on-screen performance panel (cross-platform comparison).
 *
 * Bottom-left overlay showing FPS / frame-time / CPU / GPU / draws /
 * resources, sourced from the public jce_renderer_get_gpu_stats() API so
 * the SAME numbers render on win-x64 and wasm — press P to toggle.
 */
#ifndef ES_PERF_H
#define ES_PERF_H

#include <jce/api_scene.h>
#include <jce/api.h>

void es_perf_init(JceScene *scene);
void es_perf_update(JceScene *scene, float dt, const JceInput *in);

#endif /* ES_PERF_H */
