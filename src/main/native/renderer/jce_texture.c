/*
 * jce_texture.c  Cross-platform texture loading implementation.
 *
 * Pipeline: PAK  decompress  SDL3_image  SDL_Surface  bgfx texture.
 * Handles pixel format conversion to RGBA8 for bgfx compatibility.
 */

#include "jce_texture.h"
#include "resource/pak_loader.h"
#include "core/jce_log.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <string.h>

#define LOG_TAG "jce_texture"

/* -- Internal texture registry (for size queries) ------------------ */

#define MAX_TEXTURES 256

typedef struct {
    uint16_t idx;
    uint32_t width;
    uint32_t height;
} TexEntry;

static TexEntry s_registry[MAX_TEXTURES];
static int      s_count;

static void registry_add(uint16_t idx, uint32_t w, uint32_t h)
{
    if (s_count < MAX_TEXTURES)
        s_registry[s_count++] = (TexEntry){ idx, w, h };
}

static TexEntry *registry_find(uint16_t idx)
{
    for (int i = 0; i < s_count; i++)
        if (s_registry[i].idx == idx)
            return &s_registry[i];
    return NULL;
}

static void registry_remove(uint16_t idx)
{
    for (int i = 0; i < s_count; i++) {
        if (s_registry[i].idx == idx) {
            s_registry[i] = s_registry[--s_count];
            return;
        }
    }
}

/* -- Helpers -------------------------------------------------------- */

/* Convert any SDL_Surface to RGBA8 (SDL_PIXELFORMAT_RGBA8888). */
static SDL_Surface *ensure_rgba8(SDL_Surface *src)
{
    if (!src) return NULL;

    if (src->format == SDL_PIXELFORMAT_RGBA32)
        return src;

    SDL_Surface *converted = SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(src);
    if (!converted)
        LOG_ERROR(LOG_TAG, "SDL_ConvertSurface failed: %s", SDL_GetError());
    return converted;
}

/* Map sampler mode to bgfx flags. */
static uint64_t sampler_flags(int mode)
{
    switch (mode) {
    case JCE_TEX_WRAP:
        return BGFX_TEXTURE_NONE; /* default wrap behavior */
    case JCE_TEX_MIRROR:
        return BGFX_SAMPLER_U_MIRROR | BGFX_SAMPLER_V_MIRROR;
    default: /* JCE_TEX_CLAMP */
        return BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    }
}

/* Create a bgfx texture from an RGBA8 surface. */
static JceTexture texture_from_surface_ex(SDL_Surface *surf, int mode)
{
    if (!surf) return JCE_TEXTURE_INVALID;

    uint32_t w = (uint32_t)surf->w;
    uint32_t h = (uint32_t)surf->h;
    uint32_t pitch = (uint32_t)surf->pitch;
    uint32_t expected_pitch = w * 4;

    /* bgfx expects tightly packed rows. Copy row-by-row if pitch differs. */
    const bgfx_memory_t *mem = bgfx_alloc(w * h * 4);
    if (pitch == expected_pitch) {
        memcpy(mem->data, surf->pixels, w * h * 4);
    } else {
        const uint8_t *src = (const uint8_t *)surf->pixels;
        uint8_t *dst = mem->data;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(dst, src, expected_pitch);
            src += pitch;
            dst += expected_pitch;
        }
    }

    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)w, (uint16_t)h,
        false,  /* no mipmaps */
        1,      /* layers */
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | sampler_flags(mode),
        mem);

    if (handle.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "bgfx_create_texture_2d failed (%ux%u)", w, h);
        return JCE_TEXTURE_INVALID;
    }

    registry_add(handle.idx, w, h);

    JceTexture tex;
    tex.idx = handle.idx;
    return tex;
}

/* -- Public API ----------------------------------------------------- */

JceTexture jce_texture_load_from_surface(SDL_Surface *surf, int sampler_mode)
{
    if (!surf) return JCE_TEXTURE_INVALID;
    return texture_from_surface_ex(surf, sampler_mode);
}

JceTexture jce_texture_load(PakArchive *pak, const char *asset_path)
{
    return jce_texture_load_ex(pak, asset_path, JCE_TEX_CLAMP);
}

JceTexture jce_texture_load_ex(PakArchive *pak, const char *asset_path,
                                int sampler_mode)
{
    if (!pak || !asset_path) return JCE_TEXTURE_INVALID;

    const PakAsset *asset = pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found in PAK: %s", asset_path);
        return JCE_TEXTURE_INVALID;
    }

    /* Decompress from PAK. */
    void *buf = SDL_malloc((size_t)asset->original_size);
    if (!buf) return JCE_TEXTURE_INVALID;

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        SDL_free(buf);
        return JCE_TEXTURE_INVALID;
    }

    /* Load image from memory via SDL3_image. */
    SDL_IOStream *io = SDL_IOFromConstMem(buf, (size_t)asset->original_size);
    if (!io) {
        SDL_free(buf);
        return JCE_TEXTURE_INVALID;
    }

    SDL_Surface *surf = IMG_Load_IO(io, true);  /* true = auto-close io */
    SDL_free(buf);

    if (!surf) {
        LOG_ERROR(LOG_TAG, "IMG_Load_IO failed for %s: %s",
                  asset_path, SDL_GetError());
        return JCE_TEXTURE_INVALID;
    }

    /* Convert to RGBA8 and upload. */
    surf = ensure_rgba8(surf);
    JceTexture tex = texture_from_surface_ex(surf, sampler_mode);
    SDL_DestroySurface(surf);

    if (jce_texture_valid(tex))
        LOG_DEBUG(LOG_TAG, "loaded %s", asset_path);

    return tex;
}

JceTexture jce_texture_from_rgba(const void *data,
                                  uint32_t width, uint32_t height)
{
    if (!data || width == 0 || height == 0)
        return JCE_TEXTURE_INVALID;

    const bgfx_memory_t *mem = bgfx_alloc(width * height * 4);
    memcpy(mem->data, data, width * height * 4);

    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height,
        false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        mem);

    if (handle.idx == UINT16_MAX)
        return JCE_TEXTURE_INVALID;

    registry_add(handle.idx, width, height);

    JceTexture tex;
    tex.idx = handle.idx;
    return tex;
}

void jce_texture_get_size(JceTexture tex, uint32_t *w, uint32_t *h)
{
    TexEntry *e = registry_find(tex.idx);
    if (w) *w = e ? e->width  : 0;
    if (h) *h = e ? e->height : 0;
}

void jce_texture_destroy(JceTexture tex)
{
    if (!jce_texture_valid(tex)) return;
    bgfx_texture_handle_t handle;
    handle.idx = tex.idx;
    bgfx_destroy_texture(handle);
    registry_remove(tex.idx);
}
