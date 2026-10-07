#include "probes.h"
#include <string.h>

#define LOG_TAG "benchmark_probe"

void bench_probe_sample(const char *name, const BenchProbeConfig *config,
                        uint32_t round, uint64_t started, uint64_t ended,
                        uint64_t completed, uint64_t checksum, bool correct)
{
    /* Log only after the timed region; retain integer clocks and work counts. */
    LOG_INFO(LOG_TAG, "benchmark-probe: name=%s round=%u count=%u completed=%llu "
             "concurrency=%u depth=%u ticks=%llu frequency=%llu checksum=%llu correct=%u",
             name, round, config->count, (unsigned long long)completed,
             config->concurrency, config->depth, (unsigned long long)(ended - started),
             (unsigned long long)jce_time_perf_freq(), (unsigned long long)checksum,
             correct ? 1u : 0u);
}

bool bench_probe_run(const char *name, const BenchProbeConfig *config)
{
    if (!name || !config || !config->count || config->count > 2000000u ||
        !config->rounds || config->rounds > 100u || !config->concurrency ||
        config->concurrency > 128u || !config->depth || config->depth > 64u)
        return false;
    if (!strcmp(name, "jobs") || !strcmp(name, "transform") ||
        !strcmp(name, "hierarchy") || !strcmp(name, "scene-load"))
        return bench_probe_core(name, config);
    if (!strcmp(name, "raycast") || !strcmp(name, "pathfinding") ||
        !strcmp(name, "behavior-tree"))
        return bench_probe_ai_physics(name, config);
    if (!strcmp(name, "physics-contacts") || !strcmp(name, "physics-disabled"))
        return bench_probe_contacts(name, config);
    if (!strcmp(name, "animation") || !strcmp(name, "animation-blend"))
        return bench_probe_animation(name, config);
    if (!strcmp(name, "world-stream"))
        return bench_probe_streaming(name, config);
    if (!strcmp(name, "ui-layout"))
        return bench_probe_ui(name, config);
    LOG_ERROR(LOG_TAG, "unknown benchmark probe: %s", name);
    return false;
}
