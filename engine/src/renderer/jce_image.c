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

/* TGA has no required magic, so SDL_image only loads it with a type hint.
 * Admit only the supported header/payload structure before supplying that hint.
 * This is a bounds/packet admission check, not another pixel decoder: SDL_image
 * still owns colour conversion, orientation, palette and RLE pixel decoding. */
static bool img_tga_reject(const char **reason, const char *detail)
{
    if (reason)
        *reason = detail;
    return false;
}

static bool img_tga_admitted(const void *data, size_t size, const char **reason)
{
    const uint8_t *b = (const uint8_t *)data;
    if (reason)
        *reason = NULL;
    if (size < 18u)
        return false;
    const unsigned type = b[2];
    const bool indexed = type == 1u || type == 9u;
    const bool colour = type == 2u || type == 10u;
    const bool grey = type == 3u || type == 11u;
    const bool rle = type == 9u || type == 10u || type == 11u;
    const unsigned depth = b[16];
    const unsigned attributes = b[17] & 15u;
    const unsigned palette_first = (unsigned)b[3] | ((unsigned)b[4] << 8);
    const unsigned palette_count = (unsigned)b[5] | ((unsigned)b[6] << 8);
    const unsigned width = (unsigned)b[12] | ((unsigned)b[13] << 8);
    const unsigned height = (unsigned)b[14] | ((unsigned)b[15] << 8);
    if (!indexed && !colour && !grey)
        return false;
    if (b[1] > 1u || !width || !height || (b[17] & 0xD0u))
        return img_tga_reject(reason, "TGA header/origin/interleave unsupported");
    const uint64_t rgba_bytes = (uint64_t)width * height * 4u;
    const uint64_t rgba_pitch = (uint64_t)width * 4u;
    /* Before palette/RLE scan or decoder allocation: SDL surfaces use int
     * pitches, and the public byte decoder already limits input to INT_MAX. */
    if (rgba_bytes > (uint64_t)INT_MAX || rgba_pitch > (uint64_t)INT_MAX ||
        rgba_bytes > (uint64_t)SIZE_MAX)
        return img_tga_reject(reason, "TGA decoded RGBA bytes/pitch exceeds INT_MAX");
    /* The pinned SDL XRGB1555 path discards the attribute alpha bit. */
    if (colour && (depth == 15u || depth == 16u) && attributes != 0u)
        return img_tga_reject(reason, "TGA 15/16-bit attribute alpha unsupported");
    if ((indexed && (b[1] != 1u || depth != 8u || attributes != 0u)) ||
        (grey && (b[1] != 0u || depth != 8u || attributes != 0u)) ||
        (colour && !((depth == 15u || depth == 16u) ? attributes <= 1u :
                     depth == 24u ? attributes == 0u :
                     depth == 32u && (attributes == 0u || attributes == 8u))))
        return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
    if (b[1]) {
        if (!palette_count || (b[7] != 15u && b[7] != 16u &&
                               b[7] != 24u && b[7] != 32u))
            return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
        /* The shipped SDL palette loader starts at entry zero. Nonzero starts
         * are not a supported subset; do not return silently shifted colours. */
        if (indexed && (palette_first != 0u || palette_count > 256u))
            return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
    } else if (palette_first || palette_count || b[7]) {
        return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
    }
    size_t cursor = 18u;
    if ((size_t)b[0] > size - cursor)
        return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
    cursor += b[0];
    const size_t palette_bytes = b[1] ? (size_t)palette_count * ((b[7] + 7u) / 8u) : 0u;
    if (palette_bytes > size - cursor)
        return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
    if (indexed && b[7] == 32u) {
        /* SDL reduces palette alpha to one binary colourkey. Fully opaque
         * palettes are faithful; any authored transparency must fail closed. */
        for (unsigned i = 0; i < palette_count; ++i)
            if (b[cursor + (size_t)i * 4u + 3u] != 255u)
                return img_tga_reject(reason, "TGA 32-bit palette alpha unsupported");
    }
    cursor += palette_bytes;
    const size_t pixel_bytes = (depth + 7u) / 8u;
    uint64_t remaining = (uint64_t)width * height;
    if (remaining > (uint64_t)SIZE_MAX / 4u)
        return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
    if (!rle) {
        if (remaining > (uint64_t)(size - cursor) / pixel_bytes)
            return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
        if (indexed) {
            for (uint64_t i = 0; i < remaining; ++i)
                if (b[cursor + (size_t)i] >= palette_count)
                    return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
        }
        return true;
    }
    while (remaining) {
        if (cursor == size)
            return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
        const unsigned packet = b[cursor++];
        const unsigned count = (packet & 127u) + 1u;
        if ((uint64_t)count > remaining)
            return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
        const unsigned stored_pixels = (packet & 128u) ? 1u : count;
        const size_t packet_bytes = (size_t)stored_pixels * pixel_bytes;
        if (packet_bytes > size - cursor)
            return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
        if (indexed) {
            for (unsigned i = 0; i < stored_pixels; ++i)
                if (b[cursor + i] >= palette_count)
                    return img_tga_reject(reason, "TGA structure/payload invalid or truncated");
        }
        cursor += packet_bytes;
        remaining -= count;
    }
    return true;
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
    /* Preserve auto-detection priority for every signature-bearing format.
     * IMG_Load_IO closes its stream even on failure; a typed retry owns a new
     * stream and also closes it exactly once through SDL_image. */
    SDL_Surface *surf = IMG_Load_IO(io, true);
    const char *decode_stage = "auto";
    const char *tga_reason = NULL;
    const bool tga_admitted = !surf && img_tga_admitted(data, (size_t)size, &tga_reason);
    if (tga_reason)
        decode_stage = "tga-admission";
    if (tga_admitted) {
        decode_stage = "tga-typed";
        io = SDL_IOFromConstMem(data, (size_t)size);
        if (!io) {
            LOG_ERROR(LOG_TAG, "image stream failed: stage=%s bytes=%llu reason=%s",
                      decode_stage, (unsigned long long)size, SDL_GetError());
            return NULL;
        }
        surf = IMG_LoadTyped_IO(io, true, "TGA");
    }
    if (!surf) {
        LOG_ERROR(LOG_TAG, "image decode failed: stage=%s bytes=%llu reason=%s",
                  decode_stage, (unsigned long long)size,
                  tga_reason ? tga_reason : SDL_GetError());
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
