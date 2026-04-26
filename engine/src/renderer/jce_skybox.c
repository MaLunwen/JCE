/*
 * jce_skybox.c  Equirectangular HDR skybox loading and cubemap rendering.
 *
 * Pipeline:
 *   1. Load equirect HDR image via SDL3_image → RGBA16F texture
 *   2. Convert equirect to cubemap via 6-pass GPU rendering (TODO: Phase 1B)
 *   3. Render cubemap skybox as fullscreen quad reconstructing view ray
 *
 * For now (Phase 1A), we load the equirect HDR and store it as a flat
 * texture. Cubemap conversion and skybox rendering will be added when
 * the equirect-to-cube shader is compiled.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_skybox.h>

#include "internal/stb_image.h"
#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "jce_skybox"

/* MSVC C mode cannot `return { ... }` directly; use a helper. */
static bgfx_texture_handle_t sky_invalid_tex_handle(void)
{
    bgfx_texture_handle_t h = BGFX_INVALID_HANDLE;
    return h;
}

struct JceSkybox {
    bgfx_texture_handle_t equirect_tex;  /* RGBA16F equirect */
    bgfx_texture_handle_t cubemap_tex;   /* 6-face cubemap (may be invalid) */
    uint32_t              equirect_w;
    uint32_t              equirect_h;
    uint32_t              cubemap_size;
};

/* ================================================================== */
/* Capability check                                                    */
/* ================================================================== */

bool jce_skybox_supported(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    /* Need RGBA16F texture support. */
    uint16_t flags = caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F];
    return (flags & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0;
}

/* ================================================================== */
/* HDR loading helpers                                                 */
/* ================================================================== */

/* Convert a float32 pixel buffer to float16 (half) using bit manipulation.
 * Simple conversion: just truncate mantissa, no rounding. */
static uint16_t float_to_half(float f)
{
    union { float f; uint32_t u; } conv;
    conv.f = f;
    uint32_t b = conv.u;
    uint32_t sign   = (b >> 16) & 0x8000;
    int32_t  expo   = ((int32_t)((b >> 23) & 0xFF)) - 127 + 15;
    uint32_t mantissa = (b >> 13) & 0x03FF;

    if (expo <= 0) return (uint16_t)sign;             /* underflow → 0 */
    if (expo >= 31) return (uint16_t)(sign | 0x7C00);  /* overflow → inf */
    return (uint16_t)(sign | ((uint32_t)expo << 10) | mantissa);
}

/* Load an HDR file and create a RGBA16F bgfx texture.
 * Uses stb_image for HDR decoding (SDL3_image lacks HDR support). */
static bgfx_texture_handle_t load_hdr_texture(const char *path,
                                               const void *mem_data,
                                               uint32_t mem_size,
                                               uint32_t *out_w,
                                               uint32_t *out_h)
{
    bgfx_texture_handle_t invalid = BGFX_INVALID_HANDLE;

    int w = 0, h = 0, channels = 0;
    float *pixels = NULL;

    if (path) {
        /* Read the file into memory first (stbi_loadf needs stdio which
         * we've disabled; use stbi_loadf_from_memory instead). */
        size_t fsize = 0;
        unsigned char *fbuf = (unsigned char *)jce_fs_host_read_all(path, &fsize);
        if (!fbuf) {
            LOG_WARN(LOG_TAG, "failed to open HDR file: %s", path);
            return invalid;
        }
        if (fsize == 0) { JCE_FREE(fbuf); return invalid; }

        pixels = stbi_loadf_from_memory(fbuf, (int)fsize, &w, &h, &channels, 4);
        JCE_FREE(fbuf);
    } else if (mem_data && mem_size > 0) {
        pixels = stbi_loadf_from_memory(
            (const stbi_uc *)mem_data, (int)mem_size, &w, &h, &channels, 4);
    }

    if (!pixels || w <= 0 || h <= 0) {
        LOG_WARN(LOG_TAG, "failed to load HDR image: %s",
                 path ? path : "(memory)");
        if (pixels) stbi_image_free(pixels);
        return invalid;
    }

    if (out_w) *out_w = (uint32_t)w;
    if (out_h) *out_h = (uint32_t)h;

    /* Convert float32 RGBA to float16 RGBA for GPU upload. */
    uint32_t pixel_count = (uint32_t)(w * h);
    uint32_t half_size = pixel_count * 4 * sizeof(uint16_t);
    uint16_t *half_data = (uint16_t *)JCE_MALLOC(half_size);
    if (!half_data) {
        stbi_image_free(pixels);
        return invalid;
    }

    for (uint32_t i = 0; i < pixel_count * 4; i++) {
        half_data[i] = float_to_half(pixels[i]);
    }
    stbi_image_free(pixels);

    const bgfx_memory_t *bgfx_mem = bgfx_copy(half_data, half_size);
    JCE_FREE(half_data);

    bgfx_texture_handle_t tex = bgfx_create_texture_2d(
        (uint16_t)w, (uint16_t)h, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        bgfx_mem);

    if (BGFX_HANDLE_IS_VALID(tex))
        LOG_INFO(LOG_TAG, "HDR texture loaded: %dx%d (%s)",
                 w, h, path ? path : "memory");

    return tex;
}

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

JceSkybox *jce_skybox_create_from_hdr_file(const char *path,
                                            uint32_t cubemap_size)
{
    if (!path) return NULL;
    if (!jce_skybox_supported()) {
        LOG_WARN(LOG_TAG, "HDR skybox not supported (no float texture caps)");
        return NULL;
    }

    JceSkybox *sky = (JceSkybox *)JCE_CALLOC(1, sizeof(*sky));
    if (!sky) return NULL;

    sky->cubemap_size = cubemap_size > 0 ? cubemap_size : 512;
    sky->cubemap_tex.idx = UINT16_MAX;

    sky->equirect_tex = load_hdr_texture(path, NULL, 0,
                                          &sky->equirect_w,
                                          &sky->equirect_h);
    if (!BGFX_HANDLE_IS_VALID(sky->equirect_tex)) {
        JCE_FREE(sky);
        return NULL;
    }

    return sky;
}

JceSkybox *jce_skybox_create_from_hdr_memory(const void *data, uint32_t data_size,
                                              uint32_t cubemap_size)
{
    if (!data || data_size == 0) return NULL;
    if (!jce_skybox_supported()) return NULL;

    JceSkybox *sky = (JceSkybox *)JCE_CALLOC(1, sizeof(*sky));
    if (!sky) return NULL;

    sky->cubemap_size = cubemap_size > 0 ? cubemap_size : 512;
    sky->cubemap_tex.idx = UINT16_MAX;

    sky->equirect_tex = load_hdr_texture(NULL, data, data_size,
                                          &sky->equirect_w,
                                          &sky->equirect_h);
    if (!BGFX_HANDLE_IS_VALID(sky->equirect_tex)) {
        JCE_FREE(sky);
        return NULL;
    }

    return sky;
}

void jce_skybox_destroy(JceSkybox *sky)
{
    if (!sky) return;
    if (BGFX_HANDLE_IS_VALID(sky->equirect_tex))
        bgfx_destroy_texture(sky->equirect_tex);
    if (BGFX_HANDLE_IS_VALID(sky->cubemap_tex))
        bgfx_destroy_texture(sky->cubemap_tex);
    JCE_FREE(sky);
}

/* ================================================================== */
/* Rendering (equirect projection for now)                             */
/* ================================================================== */

void jce_skybox_render(const JceSkybox *sky, uint16_t view_id,
                       const jce_mat4 *inv_vp, float exposure)
{
    /* Skybox rendering is done by the editor's sky shader (fs_sky.sc)
     * when cubemap conversion is available. For now, this is a placeholder
     * that will be connected when the equirect-to-cubemap shader pipeline
     * and cubemap sky shader are compiled and integrated. */
    (void)sky;
    (void)view_id;
    (void)inv_vp;
    (void)exposure;
}

/* ================================================================== */
/* Accessors                                                           */
/* ================================================================== */

JceTexture jce_skybox_get_equirect_texture(const JceSkybox *sky)
{
    if (!sky) return JCE_TEXTURE_INVALID;
    return (JceTexture){ sky->equirect_tex.idx };
}

JceTexture jce_skybox_get_cubemap(const JceSkybox *sky)
{
    if (!sky) return JCE_TEXTURE_INVALID;
    return (JceTexture){ sky->cubemap_tex.idx };
}
