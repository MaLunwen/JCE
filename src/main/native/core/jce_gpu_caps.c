/*
 * jce_gpu_caps.c  GPU capability querying implementation.
 */

#include "jce_gpu_caps.h"
#include "jce_log.h"

#include <bgfx/c99/bgfx.h>
#include <string.h>

#define LOG_TAG "jce_gpu_caps"

static const char *vendor_string(uint16_t id)
{
    switch (id) {
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x8086: return "Intel";
    case 0x13B5: return "ARM";
    case 0x106B: return "Apple";
    case 0x5143: return "Qualcomm";
    default:     return "Unknown";
    }
}

void jce_gpu_caps_init(JceGpuCaps *caps)
{
    if (!caps) return;
    memset(caps, 0, sizeof(*caps));

    const bgfx_caps_t *bc = bgfx_get_caps();
    if (!bc) return;

    /* Vendor / device. */
    caps->vendor_id     = bc->vendorId;
    caps->vendor_name   = vendor_string(bc->vendorId);
    caps->renderer_name = bgfx_get_renderer_name(bgfx_get_renderer_type());
    caps->homogeneous_ndc = bc->homogeneousDepth;

    /* Features (check bgfx supported flags). */
    caps->supports_compute          = (bc->supported & BGFX_CAPS_COMPUTE)            != 0;
    caps->supports_instancing       = (bc->supported & BGFX_CAPS_INSTANCING)         != 0;
    caps->supports_indirect         = (bc->supported & BGFX_CAPS_DRAW_INDIRECT)      != 0;
    caps->supports_occlusion_query  = (bc->supported & BGFX_CAPS_OCCLUSION_QUERY)    != 0;
    caps->supports_texture_2d_array = (bc->supported & BGFX_CAPS_TEXTURE_2D_ARRAY)   != 0;
    caps->supports_texture_cube_array = (bc->supported & BGFX_CAPS_TEXTURE_CUBE_ARRAY) != 0;

    /* Texture compression formats. */
    caps->supports_texture_bc   = (bc->formats[BGFX_TEXTURE_FORMAT_BC1]
                                    & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0;
    caps->supports_texture_etc  = (bc->formats[BGFX_TEXTURE_FORMAT_ETC2]
                                    & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0;
    caps->supports_texture_astc = (bc->formats[BGFX_TEXTURE_FORMAT_ASTC4X4]
                                    & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0;

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
    LOG_INFO(LOG_TAG, "compute=%s  instancing=%s  indirect=%s  msaa=%s",
             caps->supports_compute   ? "yes" : "no",
             caps->supports_instancing ? "yes" : "no",
             caps->supports_indirect   ? "yes" : "no",
             caps->supports_msaa       ? "yes" : "no");
    LOG_INFO(LOG_TAG, "tex_bc=%s  tex_etc=%s  tex_astc=%s  ndc=%s",
             caps->supports_texture_bc   ? "yes" : "no",
             caps->supports_texture_etc  ? "yes" : "no",
             caps->supports_texture_astc ? "yes" : "no",
             caps->homogeneous_ndc       ? "[-1,1]" : "[0,1]");
    LOG_INFO(LOG_TAG, "max_tex=%u  max_draws=%u  max_views=%u  max_fb_attach=%u",
             caps->max_texture_size, caps->max_draw_calls,
             caps->max_views, caps->max_fb_attachments);
    if (caps->gpu_memory_bytes > 0) {
        LOG_INFO(LOG_TAG, "gpu_mem=%lld MB",
                 (long long)(caps->gpu_memory_bytes / (1024 * 1024)));
    }
}
