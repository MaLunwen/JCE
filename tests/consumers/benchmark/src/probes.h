/* Project-owned, installed-SDK diagnostic workloads. */
#ifndef BENCHMARK_PROBES_H
#define BENCHMARK_PROBES_H

#include <jce/api_core.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct BenchProbeConfig {
    uint32_t count;
    uint32_t rounds;
    uint32_t concurrency;
    uint32_t depth;
} BenchProbeConfig;

bool bench_probe_run(const char *name, const BenchProbeConfig *config);
bool bench_probe_core(const char *name, const BenchProbeConfig *config);
bool bench_probe_ai_physics(const char *name, const BenchProbeConfig *config);
bool bench_probe_contacts(const char *name, const BenchProbeConfig *config);
bool bench_probe_animation(const char *name, const BenchProbeConfig *config);
bool bench_probe_streaming(const char *name, const BenchProbeConfig *config);
bool bench_probe_ui(const char *name, const BenchProbeConfig *config);
void bench_probe_sample(const char *name, const BenchProbeConfig *config,
                        uint32_t round, uint64_t started, uint64_t ended,
                        uint64_t completed, uint64_t checksum, bool correct);

#endif
