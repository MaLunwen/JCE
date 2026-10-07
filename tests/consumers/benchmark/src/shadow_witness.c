/* Reuse public shadow view modes and borrowed target enumeration only. */
#include "shadow_witness.h"
#include <jce/api_app.h>
#include <jce/api_core.h>
#include <stdlib.h>
#include <string.h>

bool bench_shadow_configure(JceSceneRenderConfig *config,
                            JceSceneRenderer *renderer, unsigned frame)
{
    const char *mode = getenv("JCE_BENCH_SHADOW_VIEW");
    const char *targets = getenv("JCE_BENCH_SHADOW_TARGETS");
    bool inventory_only = targets && !strcmp(targets, "1");
    bool view_requested = mode && mode[0];
    if (!view_requested && !inventory_only) return true;
    if (view_requested && !strcmp(mode, "mask")) config->view_mode = JCE_SCENE_VIEW_SHADOW_MASK;
    else if (view_requested && !strcmp(mode, "cascade")) config->view_mode = JCE_SCENE_VIEW_SHADOW_CASCADES;
    else if (view_requested && !strcmp(mode, "depth")) config->view_mode = JCE_SCENE_VIEW_SCENE_DEPTH;
    else if (view_requested) {
        LOG_ERROR("benchmark_shadow", "benchmark-shadow-view: invalid mode=%s", mode);
        jce_engine_request_quit();
        return false;
    }
    if (view_requested && (frame == 1 || frame == 50))
        LOG_INFO("benchmark_shadow", "benchmark-shadow-view: frame=%u mode=%s value=%u",
                 frame, mode, (unsigned)config->view_mode);
    if (frame == 50) {
        int count = jce_scene_renderer_debug_target_count(renderer);
        LOG_INFO("benchmark_shadow", "benchmark-shadow-targets: frame=%u count=%d", frame, count);
        for (int i = 0; i < count; ++i) {
            JceRenderTargetInfo info;
            if (!jce_scene_renderer_debug_target_get(renderer, i, &info)) {
                LOG_ERROR("benchmark_shadow", "benchmark-shadow-target: unavailable index=%d", i);
                jce_engine_request_quit();
                return false;
            }
            LOG_INFO("benchmark_shadow",
                "benchmark-shadow-target: frame=%u index=%d name=%s width=%u height=%u texture=%u kind=%u",
                frame, i, info.name, (unsigned)info.width, (unsigned)info.height,
                (unsigned)info.texture.idx, (unsigned)info.kind);
        }
    }
    return true;
}
