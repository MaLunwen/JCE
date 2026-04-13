/*
 * jce_gpu_caps.h  GPU capability querying (inspired by The-Forge GPUSettings).
 *
 * Wraps bgfx_get_caps() into readable booleans for feature-gating.
 * Call jce_gpu_caps_init() after bgfx_init().
 */

#ifndef JCE_GPU_CAPS_H
#define JCE_GPU_CAPS_H

#include <stdbool.h>
#include <stdint.h>

typedef struct JceGpuCaps {
    /* Vendor / device info. */
    uint16_t    vendor_id;
    const char *vendor_name;      /* "NVIDIA", "AMD", "Intel", etc. */
    const char *renderer_name;    /* "Vulkan", "Direct3D 12", etc. */

    /* Feature support. */
    bool  supports_compute;
    bool  supports_instancing;
    bool  supports_indirect;
    bool  supports_occlusion_query;
    bool  supports_texture_2d_array;
    bool  supports_texture_cube_array;
    bool  supports_msaa;
    bool  homogeneous_ndc;           /* true = [-1,1] (GL/Vulkan), false = [0,1] (D3D) */

    /* Limits (bgfx reports these as uint32_t). */
    uint32_t  max_texture_size;
    uint32_t  max_draw_calls;
    uint32_t  max_fb_attachments;
    uint32_t  max_views;

    /* GPU memory (0 if unavailable). */
    int64_t   gpu_memory_bytes;
} JceGpuCaps;

/* Query and log all GPU capabilities. Call after bgfx_init(). */
void jce_gpu_caps_init(JceGpuCaps *caps);

#endif /* JCE_GPU_CAPS_H */
