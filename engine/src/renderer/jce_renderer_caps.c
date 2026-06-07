/*
 * jce_renderer_caps.c  GPU capability tiering implementation.
 *
 * Queries bgfx_get_caps() at runtime and classifies the GPU into
 * LOW / MEDIUM / HIGH tiers.  This drives automatic quality scaling
 * for post-processing, shadow maps, and material complexity.
 */

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_renderer_caps.h>

#include <bgfx/c99/bgfx.h>
#include <stdbool.h>

#define LOG_TAG "renderer_caps"

/* ── Editor / test tier override ──────────────────────────────────── */

/* Single int read/written from the editor UI thread and read from the
   render thread.  Aligned int writes are atomic on x86/arm64, which is
   sufficient for this debug-only knob — we don't need acquire/release
   ordering because nothing else depends on it. */
static volatile int s_tier_override_active = 0;
static volatile int s_tier_override_value  = (int)JCE_GPU_TIER_HIGH;

void jce_renderer_set_tier_override(JceGpuTier tier)
{
    if ((int)tier < 0 || (int)tier >= (int)JCE_GPU_TIER_COUNT)
        return;
    s_tier_override_value  = (int)tier;
    s_tier_override_active = 1;
}

void jce_renderer_clear_tier_override(void)
{
    s_tier_override_active = 0;
}

bool jce_renderer_tier_is_overridden(void)
{
    return s_tier_override_active != 0;
}

/* ── Tier classification ──────────────────────────────────────────── */

static JceGpuTier s_detect_tier(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps)
        return JCE_GPU_TIER_LOW;

    bgfx_renderer_type_t backend = bgfx_get_renderer_type();

    /* OpenGL ES is typically mobile — start at LOW. */
    if (backend == BGFX_RENDERER_TYPE_OPENGLES)
        return JCE_GPU_TIER_LOW;

    /* Compute the tier from a simple scoring heuristic. */
    int score = 0;

    /* Modern APIs score higher. */
    if (backend == BGFX_RENDERER_TYPE_VULKAN || backend == BGFX_RENDERER_TYPE_METAL ||
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
    if (caps->vendorId == 0x10DE || /* NVIDIA */
        caps->vendorId == 0x1002)   /* AMD */
        score += 2;
    else if (caps->vendorId == 0x106B) /* Apple Silicon */
        score += 1;

    /* Large max texture size indicates desktop-class hardware. */
    if (caps->limits.maxTextureSize >= 8192)
        score += 1;

    /* Classify. */
    if (score >= 7)
        return JCE_GPU_TIER_HIGH;
    if (score >= 4)
        return JCE_GPU_TIER_MEDIUM;
    return JCE_GPU_TIER_LOW;
}

JceGpuTier jce_renderer_get_tier(void)
{
    if (s_tier_override_active)
        return (JceGpuTier)s_tier_override_value;
    return s_detect_tier();
}

/* ── Capability flags ─────────────────────────────────────────────── */

uint32_t jce_renderer_get_caps(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps)
        return 0;

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
    if (caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_2D)
        flags |= JCE_CAP_TEXTURE_FLOAT;
    if (caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER)
        flags |= JCE_CAP_FRAMEBUFFER_FLOAT;

    return flags;
}

/* ── Recommendations ──────────────────────────────────────────────── */

JceRenderRecommendation jce_renderer_get_recommendation(void)
{
    JceGpuTier tier = jce_renderer_get_tier();
    const bgfx_caps_t *caps = bgfx_get_caps();
    JceRenderRecommendation rec;

    rec.tier = tier;

    /* Hardware probes — used to gate compute / VRAM-heavy features
       independently of the coarse tier classification. */
    bool has_compute   = caps && (caps->supported & BGFX_CAPS_COMPUTE);
    bool has_tex3d     = caps && (caps->supported & BGFX_CAPS_TEXTURE_3D);
    bool has_fp_fbo    = caps &&
        (caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER);
    bool has_discrete  = caps &&
        (caps->vendorId == 0x10DE /* NVIDIA */ ||
         caps->vendorId == 0x1002 /* AMD     */);
    rec.has_discrete_gpu = has_discrete;

    switch (tier) {
    case JCE_GPU_TIER_ULTRA:
    case JCE_GPU_TIER_HIGH:
        rec.shadow_map_size = 2048;
        rec.max_postfx = 6; /* all effects */
        rec.enable_pbr = true;
        rec.enable_bloom = true;
        rec.enable_fxaa = true;
        /* Modern features ON — but still gated on hardware probes
           so the High tier of an integrated GPU behaves correctly. */
        rec.enable_ssr            = has_fp_fbo && has_discrete;
        rec.enable_ssao           = true;
        rec.enable_taa            = has_fp_fbo;
        rec.enable_volumetric_fog = has_compute && has_tex3d;
        rec.enable_gpu_particles  = has_compute;
        rec.max_texture_size = 4096;
        break;

    case JCE_GPU_TIER_MEDIUM:
        rec.shadow_map_size = 1024;
        rec.max_postfx = 3; /* tonemap + fxaa + vignette */
        rec.enable_pbr = true;
        rec.enable_bloom = false;
        rec.enable_fxaa = true;
        /* Conservative defaults.  TAA + SSAO each need extra RGBA16F targets;
           an integrated GPU that scored into MEDIUM (e.g. Intel HD) cannot
           spare that on a 512MB shared-VRAM budget, so gate them on a discrete
           GPU — mirrors the SSR gate in the HIGH tier above. */
        rec.enable_ssr            = false;
        rec.enable_ssao           = has_discrete;
        rec.enable_taa            = has_fp_fbo && has_discrete;
        rec.enable_volumetric_fog = false;
        rec.enable_gpu_particles  = has_compute;
        rec.max_texture_size = 2048;
        break;

    case JCE_GPU_TIER_LOW:
    default:
        rec.shadow_map_size = 512;
        rec.max_postfx = 1; /* tonemap only */
        rec.enable_pbr = false;
        rec.enable_bloom = false;
        rec.enable_fxaa = false;
        /* All modern features OFF.  This is the path 2008 / 2010-era
           devices and 512MB-VRAM machines take — we do NOT want a
           crash or 5fps experience there. */
        rec.enable_ssr            = false;
        rec.enable_ssao           = false;
        rec.enable_taa            = false;
        rec.enable_volumetric_fog = false;
        rec.enable_gpu_particles  = false;
        rec.max_texture_size = 1024;
        break;
    }

    LOG_INFO(LOG_TAG,
        "GPU tier: %s  shadow=%u  postfx=%u  pbr=%s  ssr=%s ssao=%s taa=%s "
        "volfog=%s gpupart=%s discrete=%s",
        jce_gpu_tier_name(tier),
        rec.shadow_map_size, rec.max_postfx,
        rec.enable_pbr            ? "on" : "off",
        rec.enable_ssr            ? "on" : "off",
        rec.enable_ssao           ? "on" : "off",
        rec.enable_taa            ? "on" : "off",
        rec.enable_volumetric_fog ? "on" : "off",
        rec.enable_gpu_particles  ? "on" : "off",
        rec.has_discrete_gpu      ? "yes" : "no");

    return rec;
}

/* ── Tier name ────────────────────────────────────────────────────── */

const char *jce_gpu_tier_name(JceGpuTier tier)
{
    switch (tier) {
    case JCE_GPU_TIER_LOW:
        return "LOW";
    case JCE_GPU_TIER_MEDIUM:
        return "MEDIUM";
    case JCE_GPU_TIER_HIGH:
        return "HIGH";
    case JCE_GPU_TIER_ULTRA:
        return "ULTRA";
    default:
        return "UNKNOWN";
    }
}

/* ── Backend enumeration ──────────────────────────────────────────── */

const char *jce_renderer_backend_name(enum JceRendererBackend b)
{
    switch (b) {
    case JCE_BACKEND_AUTO:     return "Auto";
    case JCE_BACKEND_D3D11:    return "D3D11";
    case JCE_BACKEND_D3D12:    return "D3D12";
    case JCE_BACKEND_VULKAN:   return "Vulkan";
    case JCE_BACKEND_METAL:    return "Metal";
    case JCE_BACKEND_OPENGL:   return "OpenGL";
    case JCE_BACKEND_OPENGLES: return "OpenGL ES";
    case JCE_BACKEND_NOOP:     return "Headless (NoOp)";
    default:                   return "Unknown";
    }
}

static enum JceRendererBackend s_from_bgfx(bgfx_renderer_type_t t)
{
    switch (t) {
    case BGFX_RENDERER_TYPE_DIRECT3D11: return JCE_BACKEND_D3D11;
    case BGFX_RENDERER_TYPE_DIRECT3D12: return JCE_BACKEND_D3D12;
    case BGFX_RENDERER_TYPE_VULKAN:     return JCE_BACKEND_VULKAN;
    case BGFX_RENDERER_TYPE_METAL:      return JCE_BACKEND_METAL;
    case BGFX_RENDERER_TYPE_OPENGL:     return JCE_BACKEND_OPENGL;
    case BGFX_RENDERER_TYPE_OPENGLES:   return JCE_BACKEND_OPENGLES;
    default:                            return JCE_BACKEND_AUTO;
    }
}

int jce_renderer_caps_preferred_chain(enum JceRendererBackend *out, int max)
{
    /* Single source of truth for per-platform preferred backend order.
     * Ordered from PERFORMANCE → COMPATIBILITY: modern explicit APIs
     * (D3D12 / Vulkan / Metal) come first because they have the lowest
     * CPU overhead and the best driver paths on current hardware;
     * older APIs (D3D11 / OpenGL / OpenGL ES) are listed last as
     * broadly-compatible safety nets.  When every backend in this list
     * fails to bgfx_init(), jce_renderer_create() returns NULL and the
     * engine drops to the SDL software renderer (the blue/orange
     * info-panel UI in jce_renderer_render_fallback_frame()).
     *
     * Must stay aligned with JCE_SHADER_PROFILES in the top-level
     * CMakeLists.txt (we only list backends whose .bin shaders are
     * actually built on this platform):
     *
     *   Windows : dx11 spv glsl  → D3D12 Vulkan D3D11 OpenGL
     *   macOS   : mtl  spv       → Metal Vulkan
     *   iOS/tvOS: mtl  spv       → Metal Vulkan
     *   Linux   : spv  glsl      → Vulkan OpenGL
     *   Android : essl spv       → Vulkan OpenGLES
     *   Web     : essl           → OpenGLES
     *
     * jce_renderer.c::get_platform_fallback_chain() also calls this
     * (then converts to bgfx_renderer_type_t) so runtime fallback,
     * UI dropdown and shader compilation all stay in lock-step. */
    static const enum JceRendererBackend chain[] = {
#if JCE_PLATFORM_WINDOWS
        /* D3D12 (lowest overhead, modern PSO model) →
         * Vulkan  (modern explicit API, good perf on NV/AMD) →
         * D3D11  (mature, broadest driver compatibility) →
         * OpenGL (final compatibility fallback). */
        JCE_BACKEND_D3D12, JCE_BACKEND_VULKAN, JCE_BACKEND_D3D11, JCE_BACKEND_OPENGL,
#elif JCE_PLATFORM_APPLE
        /* Metal (native, best perf) → Vulkan via MoltenVK (compat).
         * Apple deprecated desktop OpenGL; bgfx ships with
         * BGFX_CONFIG_RENDERER_OPENGL=0 on macOS and never reports
         * BGFX_RENDERER_TYPE_OPENGL.  Listing it here would only
         * pollute the editor preference dropdown with an unsupported
         * entry, so we omit GL on Apple platforms. */
        JCE_BACKEND_METAL, JCE_BACKEND_VULKAN,
#elif JCE_PLATFORM_ANDROID
        /* Vulkan (modern, perf) → OpenGL ES (universal compat). */
        JCE_BACKEND_VULKAN, JCE_BACKEND_OPENGLES,
#elif JCE_PLATFORM_WEB
        JCE_BACKEND_OPENGLES,
#elif JCE_PLATFORM_LINUX
        /* Vulkan (modern, perf) → OpenGL (compat). */
        JCE_BACKEND_VULKAN, JCE_BACKEND_OPENGL,
#else
#  error "jce_renderer_caps_preferred_chain: unknown platform — add a JCE_PLATFORM_* branch"
#endif
    };
    const int n = (int)(sizeof(chain) / sizeof(chain[0]));
    if (out && max > 0) {
        int copy = (n < max) ? n : max;
        for (int i = 0; i < copy; ++i) out[i] = chain[i];
    }
    return n;
}

int jce_renderer_caps_list_backends(enum JceRendererBackend *out, int max)
{
    bgfx_renderer_type_t supported[BGFX_RENDERER_TYPE_COUNT];
    uint8_t n = bgfx_get_supported_renderers(BGFX_RENDERER_TYPE_COUNT, supported);

    enum JceRendererBackend pref[16];
    int pref_count = jce_renderer_caps_preferred_chain(pref,
        (int)(sizeof(pref)/sizeof(pref[0])));

    enum JceRendererBackend tmp[BGFX_RENDERER_TYPE_COUNT + 1];
    int  count = 0;
    bool seen[BGFX_RENDERER_TYPE_COUNT + 1] = {0};

    tmp[count++] = JCE_BACKEND_AUTO;

    /* Pass 1: walk preferred order, append every supported entry. */
    for (int p = 0; p < pref_count; ++p) {
        for (uint8_t i = 0; i < n; ++i) {
            if (supported[i] == BGFX_RENDERER_TYPE_NOOP) continue;
            enum JceRendererBackend b = s_from_bgfx(supported[i]);
            if (b == JCE_BACKEND_AUTO) continue;
            if (b != pref[p]) continue;
            if ((int)b < (int)(sizeof(seen)/sizeof(seen[0])) && seen[(int)b]) continue;
            tmp[count++] = b;
            if ((int)b < (int)(sizeof(seen)/sizeof(seen[0]))) seen[(int)b] = true;
            break;
        }
    }

    if (out && max > 0) {
        int copy = (count < max) ? count : max;
        for (int i = 0; i < copy; ++i) out[i] = tmp[i];
    }
    return count;
}
