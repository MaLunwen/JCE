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

/* (Helper bgfx_texture_handle_t returning BGFX_INVALID_HANDLE removed —
   call-sites now construct it inline as a local with the canonical macro.) */

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
 * Uses stb_image for HDR decoding (SDL3_image lacks HDR support).
 * Kept for compatibility — new code uses load_hdr_pixels + upload_equirect_rgba16f. */

/* Load HDR float pixels (RGBA32F).  Caller must stbi_image_free().
   Returns NULL on failure. */
static float *load_hdr_pixels(const char *path,
                              const void *mem_data,
                              uint32_t mem_size,
                              int *out_w, int *out_h)
{
    int w = 0, h = 0, channels = 0;
    float *pixels = NULL;

    if (path) {
        uint64_t fsize = 0;
        unsigned char *fbuf = (unsigned char *)jce_fs_host_read_all(path, &fsize);
        if (!fbuf) { LOG_WARN(LOG_TAG, "failed to open HDR file: %s", path); return NULL; }
        if (fsize == 0) { JCE_FREE(fbuf); return NULL; }
        pixels = stbi_loadf_from_memory(fbuf, (int)fsize, &w, &h, &channels, 4);
        JCE_FREE(fbuf);
    } else if (mem_data && mem_size > 0) {
        pixels = stbi_loadf_from_memory((const stbi_uc *)mem_data,
                                        (int)mem_size, &w, &h, &channels, 4);
    }
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return NULL;
    }
    *out_w = w;
    *out_h = h;
    return pixels;
}

/* Upload an RGBA32F pixel buffer to a bgfx RGBA16F texture. */
static bgfx_texture_handle_t upload_equirect_rgba16f(const float *pixels,
                                                     int w, int h)
{
    bgfx_texture_handle_t invalid = BGFX_INVALID_HANDLE;
    uint32_t pixel_count = (uint32_t)(w * h);
    uint32_t half_size = pixel_count * 4 * sizeof(uint16_t);
    uint16_t *half_data = (uint16_t *)JCE_MALLOC(half_size);
    if (!half_data) return invalid;
    for (uint32_t i = 0; i < pixel_count * 4; i++)
        half_data[i] = float_to_half(pixels[i]);
    const bgfx_memory_t *bm = bgfx_copy(half_data, half_size);
    JCE_FREE(half_data);
    return bgfx_create_texture_2d((uint16_t)w, (uint16_t)h, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, bm);
}

/* ------------------------------------------------------------------ */
/* Equirect → Cubemap (CPU)                                           */
/* ------------------------------------------------------------------ */
/* Derive 3D direction for face/uv (bgfx face order, OpenGL convention).
   uv ∈ [-1, +1] across the face. */
static inline void cube_face_dir(int face, float u, float v,
                                 float out[3])
{
    switch (face) {
    case 0: out[0]=  1.0f; out[1]= -v;   out[2]= -u;   break; /* +X */
    case 1: out[0]= -1.0f; out[1]= -v;   out[2]=  u;   break; /* -X */
    case 2: out[0]=  u;    out[1]=  1.0f;out[2]=  v;   break; /* +Y */
    case 3: out[0]=  u;    out[1]= -1.0f;out[2]= -v;   break; /* -Y */
    case 4: out[0]=  u;    out[1]= -v;   out[2]=  1.0f;break; /* +Z */
    default:out[0]= -u;    out[1]= -v;   out[2]= -1.0f;break; /* -Z */
    }
}

static inline void normalize3(float v[3])
{
    float l = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (l <= 0.0f) return;
    l = 1.0f / (float)SDL_sqrt(l);
    v[0] *= l; v[1] *= l; v[2] *= l;
}

/* Bilinear sample equirect (RGBA32F, wrap U, clamp V). */
static inline void equirect_sample_bilinear(const float *src, int sw, int sh,
                                            float u, float v, float out[4])
{
    /* u: [0,1] horizontal, v: [0,1] vertical */
    float fx = u * (float)sw - 0.5f;
    float fy = v * (float)sh - 0.5f;
    int x0 = (int)SDL_floor(fx);
    int y0 = (int)SDL_floor(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    /* Wrap U, clamp V */
    int xw0 = ((x0 % sw) + sw) % sw;
    int xw1 = ((x1 % sw) + sw) % sw;
    int yc0 = y0 < 0 ? 0 : (y0 >= sh ? sh - 1 : y0);
    int yc1 = y1 < 0 ? 0 : (y1 >= sh ? sh - 1 : y1);

    const float *p00 = &src[(yc0 * sw + xw0) * 4];
    const float *p10 = &src[(yc0 * sw + xw1) * 4];
    const float *p01 = &src[(yc1 * sw + xw0) * 4];
    const float *p11 = &src[(yc1 * sw + xw1) * 4];
    for (int c = 0; c < 4; c++) {
        float a = p00[c] + (p10[c] - p00[c]) * tx;
        float b = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = a + (b - a) * ty;
    }
}

/* Convert equirect RGBA32F → bgfx RGBA16F cubemap (size×size per face). */
static bgfx_texture_handle_t equirect_to_cubemap_cpu(const float *src,
                                                     int sw, int sh,
                                                     uint32_t size)
{
    bgfx_texture_handle_t invalid = BGFX_INVALID_HANDLE;
    if (!src || size == 0) return invalid;

    const uint32_t face_pixels = size * size;
    const uint32_t face_bytes  = face_pixels * 4 * sizeof(uint16_t);
    const uint32_t total_bytes = face_bytes * 6;

    uint16_t *cube = (uint16_t *)JCE_MALLOC(total_bytes);
    if (!cube) return invalid;

    const float inv_size = 1.0f / (float)size;
    const float inv_pi   = 1.0f / 3.14159265358979323846f;
    const float inv_2pi  = 0.5f * inv_pi;

    for (int face = 0; face < 6; face++) {
        uint16_t *dst = &cube[face * face_pixels * 4];
        for (uint32_t y = 0; y < size; y++) {
            float v_face = ((float)y + 0.5f) * inv_size * 2.0f - 1.0f;
            for (uint32_t x = 0; x < size; x++) {
                float u_face = ((float)x + 0.5f) * inv_size * 2.0f - 1.0f;
                float dir[3];
                cube_face_dir(face, u_face, v_face, dir);
                normalize3(dir);
                /* Spherical → equirect uv. atan2 returns [-pi, +pi]. */
                float u_eq = (float)SDL_atan2(dir[2], dir[0]) * inv_2pi + 0.5f;
                float v_eq = (float)SDL_acos(dir[1]) * inv_pi;
                float rgba[4];
                equirect_sample_bilinear(src, sw, sh, u_eq, v_eq, rgba);
                uint16_t *o = &dst[(y * size + x) * 4];
                o[0] = float_to_half(rgba[0]);
                o[1] = float_to_half(rgba[1]);
                o[2] = float_to_half(rgba[2]);
                o[3] = float_to_half(rgba[3]);
            }
        }
    }

    const bgfx_memory_t *bm = bgfx_copy(cube, total_bytes);
    JCE_FREE(cube);
    return bgfx_create_texture_cube((uint16_t)size, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA16F,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP,
        bm);
}



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

    int w = 0, h = 0;
    float *pixels = load_hdr_pixels(path, NULL, 0, &w, &h);
    if (!pixels) { JCE_FREE(sky); return NULL; }

    sky->equirect_w = (uint32_t)w;
    sky->equirect_h = (uint32_t)h;
    sky->equirect_tex = upload_equirect_rgba16f(pixels, w, h);
    sky->cubemap_tex  = equirect_to_cubemap_cpu(pixels, w, h, sky->cubemap_size);
    stbi_image_free(pixels);

    if (!BGFX_HANDLE_IS_VALID(sky->equirect_tex)) {
        if (BGFX_HANDLE_IS_VALID(sky->cubemap_tex))
            bgfx_destroy_texture(sky->cubemap_tex);
        JCE_FREE(sky);
        return NULL;
    }
    LOG_INFO(LOG_TAG, "skybox: equirect %dx%d → cubemap %ux%u (%s)",
             w, h, sky->cubemap_size, sky->cubemap_size, path);
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

    int w = 0, h = 0;
    float *pixels = load_hdr_pixels(NULL, data, data_size, &w, &h);
    if (!pixels) { JCE_FREE(sky); return NULL; }

    sky->equirect_w = (uint32_t)w;
    sky->equirect_h = (uint32_t)h;
    sky->equirect_tex = upload_equirect_rgba16f(pixels, w, h);
    sky->cubemap_tex  = equirect_to_cubemap_cpu(pixels, w, h, sky->cubemap_size);
    stbi_image_free(pixels);

    if (!BGFX_HANDLE_IS_VALID(sky->equirect_tex)) {
        if (BGFX_HANDLE_IS_VALID(sky->cubemap_tex))
            bgfx_destroy_texture(sky->cubemap_tex);
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
