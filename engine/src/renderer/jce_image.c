/*
 * jce_image.c  Public image decoding API — the single dispatcher.
 *
 * LDR (PNG/JPG/TGA/BMP/WEBP/...) goes through SDL_image, which is the codec
 * set the runtime ships and the one every raw texture has always been
 * decoded with.  HDR (Radiance) and 16-bit grayscale go through the engine's
 * vendored stb_image build instead: SDL_image has no HDR support at all, and
 * its libpng path heap-overruns on 16-bit grayscale PNGs.  Which of the two
 * runs is decided HERE and nowhere else.
 */

#include <jce/renderer/jce_image.h>

#include <jce/os/core/jce_log.h>

#include <stb_image.h>
#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define LOG_TAG "jce_image"

float *jce_image_load_hdr_from_memory(const void *data, uint64_t size, int *out_w, int *out_h)
{
    if (!data || size == 0)
        return NULL;
    int w = 0, h = 0, ch = 0;
    float *pixels = stbi_loadf_from_memory((const stbi_uc *)data, (int)size, &w, &h, &ch, 4);
    if (!pixels)
        return NULL;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    return pixels;
}

void jce_image_free_hdr(float *pixels)
{
    if (pixels)
        stbi_image_free(pixels);
}

uint16_t *jce_image_load_gray16_from_memory(const void *data, uint64_t size, int *out_w, int *out_h)
{
    if (!data || size == 0 || size > (uint64_t)INT_MAX)
        return NULL;
    int w = 0, h = 0, ch = 0;
    /* req_comp=1: force single-channel; 8-bit sources promote to 0..65535. */
    stbi_us *px = stbi_load_16_from_memory((const stbi_uc *)data, (int)size, &w, &h, &ch, 1);
    if (!px)
        return NULL;
    if (w <= 0 || h <= 0) {
        stbi_image_free(px);
        return NULL;
    }
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    return (uint16_t *)px;
}

void jce_image_free_gray16(uint16_t *pixels)
{
    if (pixels)
        stbi_image_free(pixels);
}

/* 16-bit GRAYSCALE PNG (IHDR bitdepth=16, colortype=0): the bundled
 * SDL3_image/libpng path heap-overruns on this class — STATUS_HEAP_CORRUPTION,
 * originally found cooking a DCC-exported height map.  The cooker has bypassed
 * it for a while (jce_asset_cooker.c: cook_is_gray16_png), but the RUNTIME
 * decode did not, so the same file that cooked fine corrupted the heap when a
 * game loaded it raw from a PAK.  Sniffing it HERE — in the one dispatcher —
 * is what makes this file's "which decoder runs is decided here and nowhere
 * else" contract actually true for every caller. */
static bool img_is_gray16_png(const void *data, uint64_t size)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    const uint8_t *b = (const uint8_t *)data;
    return size >= 26 &&
           memcmp(b, sig, 8) == 0 &&
           memcmp(b + 12, "IHDR", 4) == 0 &&
           b[24] == 16 /* bit depth */ &&
           b[25] == 0  /* colour type: greyscale */;
}

uint8_t *jce_image_load_rgba8_from_memory(const void *data, uint64_t size, int *out_w, int *out_h)
{
    if (!data || size == 0 || size > (uint64_t)INT_MAX)
        return NULL;

    /* Route the libpng-hostile class to stb, which down-converts 16->8 with
     * the correct 2-byte stride.  req_comp=4 gives us RGBA8 directly. */
    if (img_is_gray16_png(data, size)) {
        int w = 0, h = 0, ch = 0;
        stbi_uc *px = stbi_load_from_memory((const stbi_uc *)data, (int)size,
                                            &w, &h, &ch, 4);
        if (!px)
            return NULL;
        if (w <= 0 || h <= 0) {
            stbi_image_free(px);
            return NULL;
        }
        /* Hand back an ENGINE-allocated buffer so jce_image_free_rgba8 (JCE_FREE)
         * is correct for every return path of this function, whichever decoder
         * produced the pixels. */
        const size_t bytes = (size_t)w * (size_t)h * 4u;
        uint8_t *pixels = (uint8_t *)JCE_MALLOC(bytes);
        if (!pixels) {
            stbi_image_free(px);
            return NULL;
        }
        memcpy(pixels, px, bytes);
        stbi_image_free(px);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        return pixels;
    }

    SDL_IOStream *io = SDL_IOFromConstMem(data, (size_t)size);
    if (!io)
        return NULL;
    SDL_Surface *surf = IMG_Load_IO(io, true);   /* true = auto-close io */
    if (!surf) {
        LOG_ERROR(LOG_TAG, "IMG_Load_IO failed: %s", SDL_GetError());
        return NULL;
    }
    if (surf->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_Surface *converted = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surf);
        if (!converted) {
            LOG_ERROR(LOG_TAG, "SDL_ConvertSurface failed: %s", SDL_GetError());
            return NULL;
        }
        surf = converted;
    }
    if (surf->w <= 0 || surf->h <= 0) {
        SDL_DestroySurface(surf);
        return NULL;
    }

    /* Copy out of SDL's heap into the engine allocator, tightly packing the
     * rows (a decoded surface may carry a padded pitch).  Handing the caller
     * an SDL-owned surface is what invites the cross-allocator free, so the
     * surface never escapes this function. */
    const size_t row_bytes = (size_t)surf->w * 4u;
    uint8_t *pixels = (uint8_t *)JCE_MALLOC(row_bytes * (size_t)surf->h);
    if (!pixels) {
        SDL_DestroySurface(surf);
        return NULL;
    }
    const uint8_t *src = (const uint8_t *)surf->pixels;
    for (int y = 0; y < surf->h; ++y)
        memcpy(pixels + (size_t)y * row_bytes,
               src + (size_t)y * (size_t)surf->pitch, row_bytes);

    if (out_w)
        *out_w = surf->w;
    if (out_h)
        *out_h = surf->h;
    SDL_DestroySurface(surf);
    return pixels;
}

void jce_image_free_rgba8(uint8_t *pixels)
{
    if (pixels)
        JCE_FREE(pixels);
}
