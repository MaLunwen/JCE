/* Renderer-only object state; the existing lifecycle bridge exposes binding. */
#ifndef JCE_RENDERER_LAUNCH_POLICY_INTERNAL_H
#define JCE_RENDERER_LAUNCH_POLICY_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
typedef struct JceRendererLaunchVsync {
    bool present, value, requested, available, bound;
    bool observed, observed_requested, observed_effective;
    bool window_flags_valid, hidden_d3d12;
    uint32_t window_flags;
} JceRendererLaunchVsync;
/* Private, immutable per-create diagnostic request. Zero preserves bgfx defaults. */
typedef struct JceRendererLaunchDiagnostics {
    bool flip_present, latency_present;
    uint8_t flip, latency, invalid;
} JceRendererLaunchDiagnostics;
#endif
