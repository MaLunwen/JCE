#ifndef BENCHMARK_HIZ_PROBE_H
#define BENCHMARK_HIZ_PROBE_H

#include "gpu_survivors.h"
#include <jce/api_resource.h>

typedef struct BenchHizProbe {
    JceGpuScene *scene;
    JceTextureHandle depth[6];
    uint16_t width, height;
    bool coarse;
    bool active;
} BenchHizProbe;

bool bench_hiz_probe_create(BenchHizProbe *probe, const JcePakArchive *pak);
bool bench_hiz_probe_draw(BenchHizProbe *probe, BenchGpuSurvivors *survivors, unsigned frame);
void bench_hiz_probe_destroy(BenchHizProbe *probe);

#endif
