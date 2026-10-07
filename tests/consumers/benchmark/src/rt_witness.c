#include "rt_witness.h"
#include <jce/api_core.h>
#include <stdlib.h>
#include <string.h>

static const char *requested_target;

bool bench_rt_enabled(void)
{
    return requested_target != NULL;
}

bool bench_rt_configure(void)
{
    JceRenderPipelineDesc pipeline;
    JceCvar *taa;
    requested_target = getenv("JCE_BENCH_RT_TARGET");
    if (!requested_target || !requested_target[0]) {
        requested_target = NULL;
        return true;
    }
    if (strcmp(requested_target, "gbuffer.normal") &&
        strcmp(requested_target, "gbuffer.albedo") &&
        strcmp(requested_target, "reflection.ssr")) return false;
    jce_render_pipeline_get(&pipeline);
    pipeline.enable_taa = false;
    pipeline.enable_ssao = false;
    pipeline.enable_ssr = true;
    pipeline.enable_motion_blur = false;
    pipeline.enable_volumetric_fog = false;
    jce_render_pipeline_apply(&pipeline);
    taa = jce_cvar_find("r.taa");
    if (!taa) return false;
    jce_cvar_set_bool(taa, false);
    return true;
}

JceTextureHandle bench_rt_texture(JceSceneRenderer *renderer,
                                 JceTextureHandle fallback)
{
    int count;
    JceTextureHandle invalid = {UINT16_MAX};
    if (!requested_target) return fallback;
    count = jce_scene_renderer_debug_target_count(renderer);
    for (int i = 0; i < count; ++i) {
        JceRenderTargetInfo info;
        if (jce_scene_renderer_debug_target_get(renderer, i, &info) &&
            !strcmp(info.name, requested_target)) {
            LOG_INFO("benchmark_rt", "intermediate target: %s %ux%u",
                     info.name, (unsigned)info.width, (unsigned)info.height);
            return info.texture;
        }
    }
    LOG_ERROR("benchmark_rt", "intermediate target unavailable: %s", requested_target);
    return invalid;
}
