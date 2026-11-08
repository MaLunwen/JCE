/* Opt-in SDK diagnostic. The ordinary Engine owns every GPU submit. */
#ifndef BENCH_PROBE_CAPTURE_WITNESS_H
#define BENCH_PROBE_CAPTURE_WITNESS_H
#include <jce/api_app.h>
#include <jce/api_scene.h>
#include <jce/api_render.h>
bool bench_probe_capture_prepare(JceScene *scene);
/* True yields the entire draw, including a terminal/cancel frame. */
bool bench_probe_capture_draw(const JceServices *services, JceScene *scene,
                              JceSceneRenderer *renderer, unsigned frame);
void bench_probe_capture_exit(void);
#endif
