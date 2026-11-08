#ifndef BENCHMARK_METRICS_H
#define BENCHMARK_METRICS_H

#include <jce/api_app.h>
#include <jce/api_scene.h>
#include <jce/api_render.h>

void bench_metrics_sample(const JceServices *services, JceScene *scene,
                          JceSceneRenderer *renderer, unsigned frame,
                          double simulation_seconds, const char *event);
/* Same collector, stock draw counter and unavailable simulation clock.
 * Borrowed scene/renderer/services must remain live during this call only. */
void bench_metrics_sample_draw(const JceServices *services,JceScene *scene,
    JceSceneRenderer *renderer,unsigned frame,const char *event);
bool bench_metrics_self_test(void);

#endif
