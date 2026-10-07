#ifndef BENCHMARK_METRICS_H
#define BENCHMARK_METRICS_H

#include <jce/api_scene.h>
#include <jce/api_render.h>

void bench_metrics_sample(JceScene *scene, JceSceneRenderer *renderer,
                          unsigned frame, double simulation_seconds, const char *event);

#endif
