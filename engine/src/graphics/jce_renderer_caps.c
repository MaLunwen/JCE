/*
 * jce_renderer_caps.c  GPU capability tiering implementation.
 *
 * Queries bgfx_get_caps() at runtime and classifies the GPU into
 * LOW / MEDIUM / HIGH tiers.  This drives automatic quality scaling
 * for post-processing, shadow maps, and material complexity.
 */

#include <jce/graphics/jce_renderer_caps.h>
#include <jce/core/jce_log.h>

#include <bgfx/c99/bgfx.h>

#define LOG_TAG "renderer_caps"

/* ── Tier classification ──────────────────────────────────────────── */

JceGpuTier jce_renderer_get_tier(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps) return JCE_GPU_TIER_LOW;

    bgfx_renderer_type_t backend = bgfx_get_renderer_type();

    /* OpenGL ES is typically mobile — start at LOW. */
    if (backend == BGFX_RENDERER_TYPE_OPENGLES)
        return JCE_GPU_TIER_LOW;

    /* Compute the tier from a simple scoring heuristic. */
    int score = 0;

    /* Modern APIs score higher. */
    if (backend == BGFX_RENDERER_TYPE_VULKAN ||
        backend == BGFX_RENDERER_TYPE_METAL  ||
        backend == BGFX_RENDERER_TYPE_DIRECT3D12)
        score += 3;
    else if (backend == BGFX_RENDERER_TYPE_DIRECT3D11)
        score += 2;
    else if (backend == BGFX_RENDERER_TYPE_OPENGL)
        score += 1;

    /* Compute shader support indicates a more capable GPU. */
    if (caps->supported & BGFX_CAPS_COMPUTE)
        score += 2;

    /* Instancing. */
    if (caps->supported & BGFX_CAPS_INSTANCING)
        score += 1;

    /* Float framebuffers (needed for HDR / bloom). */
    if (caps->supported & BGFX_CAPS_TEXTURE_2D_ARRAY)
        score += 1;

    /* Draw indirect (advanced batching). */
    if (caps->supported & BGFX_CAPS_DRAW_INDIRECT)
        score += 1;

    /* Known discrete GPU vendors score higher. */
    if (caps->vendorId == 0x10DE ||   /* NVIDIA */
        caps->vendorId == 0x1002)     /* AMD */
        score += 2;
    else if (caps->vendorId == 0x106B) /* Apple Silicon */
        score += 1;

    /* Large max texture size indicates desktop-class hardware. */
    if (caps->limits.maxTextureSize >= 8192)
        score += 1;

    /* Classify. */
    if (score >= 7) return JCE_GPU_TIER_HIGH;
    if (score >= 4) return JCE_GPU_TIER_MEDIUM;
    return JCE_GPU_TIER_LOW;
}

/* ── Capability flags ─────────────────────────────────────────────── */

uint32_t jce_renderer_get_caps(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps) return 0;

    uint32_t flags = 0;

    if (caps->supported & BGFX_CAPS_COMPUTE)
        flags |= JCE_CAP_COMPUTE;
    if (caps->supported & BGFX_CAPS_INSTANCING)
        flags |= JCE_CAP_INSTANCING;
    if (caps->supported & BGFX_CAPS_TEXTURE_3D)
        flags |= JCE_CAP_TEXTURE_3D;
    if (caps->supported & BGFX_CAPS_TEXTURE_COMPARE_ALL)
        flags |= JCE_CAP_TEXTURE_COMPARE;
    if (caps->supported & BGFX_CAPS_VERTEX_ID)
        flags |= JCE_CAP_VERTEX_ID;
    if (caps->supported & BGFX_CAPS_DRAW_INDIRECT)
        flags |= JCE_CAP_DRAW_INDIRECT;

    /* Float texture/FBO support: check if RGBA16F is supported as both
       a texture and a framebuffer attachment. */
    if (caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] &
        BGFX_CAPS_FORMAT_TEXTURE_2D)
        flags |= JCE_CAP_TEXTURE_FLOAT;
    if (caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] &
        BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER)
        flags |= JCE_CAP_FRAMEBUFFER_FLOAT;

    return flags;
}

/* ── Recommendations ──────────────────────────────────────────────── */

JceRenderRecommendation jce_renderer_get_recommendation(void)
{
    JceGpuTier tier = jce_renderer_get_tier();
    JceRenderRecommendation rec;

    rec.tier = tier;

    switch (tier) {
    case JCE_GPU_TIER_HIGH:
        rec.shadow_map_size  = 2048;
        rec.max_postfx       = 6;     /* all effects */
        rec.enable_pbr       = true;
        rec.enable_bloom     = true;
        rec.enable_fxaa      = true;
        rec.max_texture_size = 4096;
        break;

    case JCE_GPU_TIER_MEDIUM:
        rec.shadow_map_size  = 1024;
        rec.max_postfx       = 3;     /* tonemap + fxaa + vignette */
        rec.enable_pbr       = true;
        rec.enable_bloom     = false;
        rec.enable_fxaa      = true;
        rec.max_texture_size = 2048;
        break;

    case JCE_GPU_TIER_LOW:
    default:
        rec.shadow_map_size  = 512;
        rec.max_postfx       = 1;     /* tonemap only */
        rec.enable_pbr       = false;
        rec.enable_bloom     = false;
        rec.enable_fxaa      = false;
        rec.max_texture_size = 1024;
        break;
    }

    LOG_INFO(LOG_TAG, "GPU tier: %s  shadow=%u  postfx=%u  pbr=%s",
             jce_gpu_tier_name(tier), rec.shadow_map_size,
             rec.max_postfx, rec.enable_pbr ? "on" : "off");

    return rec;
}

/* ── Tier name ────────────────────────────────────────────────────── */

const char *jce_gpu_tier_name(JceGpuTier tier)
{
    switch (tier) {
    case JCE_GPU_TIER_LOW:    return "LOW";
    case JCE_GPU_TIER_MEDIUM: return "MEDIUM";
    case JCE_GPU_TIER_HIGH:   return "HIGH";
    default:                  return "UNKNOWN";
    }
}
