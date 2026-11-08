/* Deterministic startup budgets; no device, allocator or platform dependency. */
#ifndef JCE_RENDERER_BUDGET_INTERNAL_H
#define JCE_RENDERER_BUDGET_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>

typedef struct JceRendererPoolBudget {
    uint32_t vertex_bytes;
    uint32_t index_bytes;
    uint32_t uniform_bytes; /* initial capacity per encoder, per bgfx Frame */
    uint16_t encoders;
} JceRendererPoolBudget;

/* Explicit pool overrides accept whole MiB in [1,256]; 0 means default. */
static inline uint32_t jce_renderer_pool_mb(const char *text)
{
    uint32_t mb = 0;
    if (!text || !text[0]) return 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return 0;
        mb = mb * 10u + (uint32_t)(*text - '0');
        if (mb > 256u) return 0;
    }
    return mb ? mb * 1048576u : 0;
}

static inline JceRendererPoolBudget jce_renderer_pool_budget(
    bool web, bool low_memory, int cores, uint32_t vertex_override,
    uint32_t index_override)
{
    JceRendererPoolBudget b;
    /* Native multithreaded bgfx owns two transient pool pairs; WebGL also
     * orphans full GPU capacity each frame. Keep unused startup capacity
     * bounded rather than charging every application for a large workload.
     * Larger geometry/UI workloads retain explicit 1..256 MiB overrides.
     * Encoder concurrency and authored material/asset quality are separate
     * policies; consumers must still handle transient capacity exhaustion. */
    b.vertex_bytes = (web || low_memory ? 6u : 8u) * 1048576u;
    b.index_bytes = 2u * 1048576u;
    /* bgfx grows uniform command buffers on demand. Preserve the encoder
     * pool while bounding unused initial capacity; this is not a hard cap. */
    b.uniform_bytes = 128u * 1024u;
    b.encoders = (web || low_memory || cores <= 2) ? 4u : 16u;
    if (vertex_override) b.vertex_bytes = vertex_override;
    if (index_override) b.index_bytes = index_override;
    return b;
}
#endif
