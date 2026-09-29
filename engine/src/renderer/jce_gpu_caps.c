/*
 * jce_gpu_caps.c  GPU capability querying implementation.
 */

#include "jce_gpu_caps.h"
#include "jce_gpu_vendor.h"

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_renderer_caps.h>

#include <bgfx/c99/bgfx.h>
#include <string.h>

#define LOG_TAG "jce_gpu_caps"

void jce_gpu_caps_init(JceGpuCaps *caps)
{
    if (!caps) return;
    memset(caps, 0, sizeof(*caps));

    const bgfx_caps_t *bc = bgfx_get_caps();
    if (!bc) return;

    /* Vendor / device. */
    caps->vendor_id     = bc->vendorId;
    caps->vendor_name   = jce_gpu_vendor_name(bc->vendorId);
    /* The RUNNING backend, not the one bgfx was compiled for -- its GL name
     * is a compile-time string built from the build floor. */
    caps->renderer_name = jce_renderer_running_backend_name();
    caps->homogeneous_ndc = bc->homogeneousDepth;

    /* Features (check bgfx supported flags). */
    caps->supports_compute          = (bc->supported & BGFX_CAPS_COMPUTE)            != 0;
    caps->supports_instancing       = (bc->supported & BGFX_CAPS_INSTANCING)         != 0;
    caps->supports_indirect         = (bc->supported & BGFX_CAPS_DRAW_INDIRECT)      != 0;
    caps->supports_occlusion_query  = (bc->supported & BGFX_CAPS_OCCLUSION_QUERY)    != 0;
    caps->supports_texture_2d_array = (bc->supported & BGFX_CAPS_TEXTURE_2D_ARRAY)   != 0;
    caps->supports_texture_cube_array = (bc->supported & BGFX_CAPS_TEXTURE_CUBE_ARRAY) != 0;

    /* MSAA: check if at least 4x is supported for RGBA8. */
    caps->supports_msaa = (bc->formats[BGFX_TEXTURE_FORMAT_RGBA8]
                            & BGFX_CAPS_FORMAT_TEXTURE_MSAA) != 0;

    /* Limits. */
    caps->max_texture_size   = bc->limits.maxTextureSize;
    caps->max_draw_calls     = bc->limits.maxDrawCalls;
    caps->max_fb_attachments = bc->limits.maxFBAttachments;
    caps->max_views          = bc->limits.maxViews;

    /* GPU memory. */
    const bgfx_stats_t *stats = bgfx_get_stats();
    if (stats)
        caps->gpu_memory_bytes = stats->gpuMemoryMax;

    /* Log summary. */
    LOG_INFO(LOG_TAG, "%s / %s", caps->vendor_name, caps->renderer_name);
    LOG_INFO(LOG_TAG, "compute=%s  instancing=%s  indirect=%s  msaa=%s  ndc=%s",
             caps->supports_compute   ? "yes" : "no",
             caps->supports_instancing ? "yes" : "no",
             caps->supports_indirect   ? "yes" : "no",
             caps->supports_msaa       ? "yes" : "no",
             caps->homogeneous_ndc     ? "[-1,1]" : "[0,1]");
    LOG_INFO(LOG_TAG, "max_tex=%u  max_draws=%u  max_views=%u  max_fb_attach=%u",
             caps->max_texture_size, caps->max_draw_calls,
             caps->max_views, caps->max_fb_attachments);
    if (caps->gpu_memory_bytes > 0) {
        LOG_INFO(LOG_TAG, "gpu_mem=%lld MB",
                 (long long)(caps->gpu_memory_bytes / (1024 * 1024)));
    }
}

/* ── Public GPU frame statistics (jce/renderer/jce_renderer.h) ─────── */

#include <jce/renderer/jce_renderer.h>

static double gpu_ticks_to_ms(int64_t delta, int64_t freq)
{
    if (freq <= 0) return 0.0;
    return (double)delta * 1000.0 / (double)freq;
}

bool jce_renderer_get_gpu_stats(JceGpuStats *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    const bgfx_stats_t *st = bgfx_get_stats();
    if (!st) return false;

    out->valid = true;

    out->cpu_frame_ms   = gpu_ticks_to_ms(st->cpuTimeFrame, st->cpuTimerFreq);
    out->cpu_submit_ms  = gpu_ticks_to_ms(st->cpuTimeEnd - st->cpuTimeBegin,
                                          st->cpuTimerFreq);
    out->gpu_ms         = gpu_ticks_to_ms(st->gpuTimeEnd - st->gpuTimeBegin,
                                          st->gpuTimerFreq);
    out->wait_submit_ms = gpu_ticks_to_ms(st->waitSubmit, st->cpuTimerFreq);
    out->wait_render_ms = gpu_ticks_to_ms(st->waitRender, st->cpuTimerFreq);

    out->num_draw    = st->numDraw;
    out->num_compute = st->numCompute;
    out->num_blit    = st->numBlit;

    out->backbuffer_width  = st->width;
    out->backbuffer_height = st->height;
    out->max_gpu_latency   = st->maxGpuLatency;

    out->num_textures      = st->numTextures;
    out->num_frame_buffers = st->numFrameBuffers;
    out->num_programs      = st->numPrograms;
    out->num_shaders       = st->numShaders;

    out->rt_memory_used      = st->rtMemoryUsed;
    out->texture_memory_used = st->textureMemoryUsed;
    out->gpu_memory_used     = st->gpuMemoryUsed;
    out->gpu_memory_max      = st->gpuMemoryMax;

    uint16_t nv = st->numViews;
    if (nv > JCE_GPU_MAX_VIEW_STATS) nv = JCE_GPU_MAX_VIEW_STATS;
    out->num_views = nv;
    if (st->viewStats) {
        for (uint16_t i = 0; i < nv; ++i) {
            const bgfx_view_stats_t *vs = &st->viewStats[i];
            JceGpuViewStat *dst = &out->views[i];
            dst->view_id = vs->view;
            size_t n = sizeof(dst->name) - 1;
            strncpy(dst->name, vs->name ? vs->name : "", n);
            dst->name[n] = '\0';
            dst->gpu_ms = gpu_ticks_to_ms(vs->gpuTimeEnd - vs->gpuTimeBegin,
                                          st->gpuTimerFreq);
            dst->cpu_ms = gpu_ticks_to_ms(vs->cpuTimeEnd - vs->cpuTimeBegin,
                                          st->cpuTimerFreq);
        }
    }

    return true;
}
