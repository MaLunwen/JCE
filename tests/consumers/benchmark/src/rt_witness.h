/* Opt-in public intermediate-target diagnostic for the SDK consumer. */
#ifndef BENCH_RT_WITNESS_H
#define BENCH_RT_WITNESS_H
#include <jce/api_render.h>
bool bench_rt_enabled(void);
bool bench_rt_configure(void);
JceTextureHandle bench_rt_texture(JceSceneRenderer *renderer,
                                 JceTextureHandle fallback);
#endif
