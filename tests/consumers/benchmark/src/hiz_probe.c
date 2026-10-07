/* SDK-only NPOT depth coverage witness; synthetic input is not frame timing. */
#include "hiz_probe.h"
#include <jce/api_core.h>
#include <stdlib.h>
#include <string.h>

bool bench_hiz_probe_create(BenchHizProbe *probe, const JcePakArchive *pak)
{
    probe->active = true;
    const char *shape = getenv("JCE_BENCH_HIZ_NPOT");
    probe->coarse = shape && strcmp(shape, "reduce") == 0;
    probe->width = shape && strcmp(shape, "thin") == 0 ? 1 : probe->coarse ? 10 : 5;
    probe->height = probe->coarse ? 10 : 5;
    for (unsigned i = 0; i < 6; ++i) probe->depth[i].idx = UINT16_MAX;
    probe->scene = jce_gpu_scene_create(pak, jce_allocator_default());
    if (!jce_gpu_scene_is_supported(probe->scene) ||
        !jce_gpu_scene_is_indirect(probe->scene) ||
        !jce_gpu_scene_hiz_kind(probe->scene)[0]) return false;
    for (unsigned kind = 0; kind < 6; ++kind) {
        uint8_t pixels[10 * 10 * 4];
        unsigned cx = probe->coarse ? probe->width - 1u : probe->width / 2u;
        unsigned cy = probe->coarse ? probe->height - 1u : probe->height / 2u;
        for (unsigned y = 0; y < probe->height; ++y) for (unsigned x = 0; x < probe->width; ++x) {
            bool clear = (kind == 0 && y == cy) || (kind == 2 && x == cx) ||
                         kind == 3 || (kind == 4 && x == cx && y == cy) ||
                         (kind == 5 && y == probe->height - 1u);
            unsigned at = (y * probe->width + x) * 4;
            pixels[at] = clear ? 255 : 51;
            pixels[at + 1] = pixels[at + 2] = 0;
            pixels[at + 3] = 255;
        }
        probe->depth[kind] = jce_texture_create_2d(probe->width, probe->height, false, 1,
            JCE_TEXTURE_FORMAT_RGBA8, JCE_SAMPLER_U_CLAMP | JCE_SAMPLER_V_CLAMP,
            jce_gfx_memory_copy(pixels, (uint32_t)probe->width * probe->height * 4u));
        if (probe->depth[kind].idx == UINT16_MAX) return false;
    }
    return true;
}

bool bench_hiz_probe_draw(BenchHizProbe *probe, BenchGpuSurvivors *survivors, unsigned frame)
{
    unsigned kind = frame < 50 ? 0 : frame < 79 ? 1 : frame < 80 ? 2 :
                    frame < 81 ? 3 : frame < 82 ? 4 : frame < 90 ? 5 : 1;
    jce_mat4 vp = jce_m4_identity();
    JceGpuSceneRecord records[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        records[i].world = vp;
        records[i].center[0] = i ? 0.1f : -0.1f;
        records[i].center[1] = kind == 5 && !probe->coarse ?
            (jce_gfx_caps().origin_bottom_left ? 0.8f : -0.8f) : 0.0f;
        records[i].center[2] = 0.8f;
        records[i].radius = 0.002f;
        for (unsigned axis = 0; axis < 3; ++axis) records[i].extent[axis] = 0.001f;
        if (probe->coarse) records[i].extent[0] = records[i].extent[1] = 0.75f;
    }
    const jce_vec4 planes[6] = {{1,0,0,10}, {-1,0,0,10}, {0,1,0,10},
                              {0,-1,0,10}, {0,0,1,10}, {0,0,-1,10}};
    jce_gpu_scene_set_hiz(probe->scene, probe->depth[kind].idx, &vp,
                          probe->width, probe->height, true);
    jce_gpu_scene_begin(probe->scene);
    JceGpuSceneRun run;
    if (!jce_gpu_scene_add_run(probe->scene, records, 2, 3, &run) ||
        !jce_gpu_scene_dispatch(probe->scene, 20, 21, planes)) return false;
    JceGpuSceneFrameStats stats;
    jce_gpu_scene_get_frame_stats(probe->scene, &stats);
    if (!stats.hiz_enabled || !stats.dispatch_succeeded || !stats.indirect_ready) return false;
    bench_gpu_survivors_tick_raw(survivors, probe->scene, frame);
    return !survivors->failed;
}

void bench_hiz_probe_destroy(BenchHizProbe *probe)
{
    if (!probe->active) return;
    jce_gpu_scene_destroy(probe->scene);
    for (unsigned i = 0; i < 6; ++i)
        if (probe->depth[i].idx != UINT16_MAX) jce_gfx_texture_destroy(probe->depth[i]);
    memset(probe, 0, sizeof(*probe));
}
