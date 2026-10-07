/* Typed, opt-in observation of stock SDK render targets. */
#ifndef BENCH_DEFAULT_RT_H
#define BENCH_DEFAULT_RT_H
#include <jce/api_app.h>
#include <jce/api_render.h>
bool bench_default_rt_init(const JceServices *services);
void bench_default_rt_tick(const JceSceneRenderer *renderer, unsigned frame);
void bench_default_rt_exit(void);
#endif
