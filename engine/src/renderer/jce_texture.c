/*
 * jce_texture.c  Cross-platform texture loading implementation.
 *
 * Pipeline: PAK → decompress → SDL3_image → SDL_Surface → bgfx texture.
 * Handles pixel format conversion to RGBA8 for bgfx compatibility.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/pak_loader.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_asset_format.h>

#include "os/core/jce_memory.h"
#include "resource/jce_asset_reader.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <stdbool.h>
#include <string.h>

#define LOG_TAG "jce_texture"

/* -- Internal texture registry (for size queries) ------------------ */

#define MAX_TEXTURES 4096

typedef struct {
    uint16_t idx;
    uint32_t width;
    uint32_t height;
} TexEntry;

static TexEntry s_registry[MAX_TEXTURES];
static int      s_count;
static bool     s_registry_warned_full;

static void registry_add(uint16_t idx, uint32_t w, uint32_t h)
{
    if (s_count < MAX_TEXTURES) {
        s_registry[s_count++] = (TexEntry){ idx, w, h };
        return;
    }
    if (!s_registry_warned_full) {
        s_registry_warned_full = true;
        LOG_WARN(LOG_TAG,
            "texture registry full (%d entries) — size queries will "
            "miss for new textures; raise MAX_TEXTURES or audit leaks",
            MAX_TEXTURES);
    }
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
static JceTexture texture_from_surface_ex(const SDL_Surface *surf, int mode)
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

JceTexture jce_texture_load_from_surface(const void *surface, int sampler_mode)
{
    if (!surface) return JCE_TEXTURE_INVALID;
    return texture_from_surface_ex((const SDL_Surface *)surface, sampler_mode);
}

JceTexture jce_texture_load(const JcePakArchive *pak, const char *asset_path)
{
    return jce_texture_load_ex(pak, asset_path, JCE_TEX_CLAMP);
}

static JceTexture jce_texture_load_ex_inner(const JcePakArchive *pak,
                                             const char *asset_path,
                                             int sampler_mode)
{
    if (!pak || !asset_path) return JCE_TEXTURE_INVALID;

    const JcePakAsset *asset = jce_pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found in PAK: %s", asset_path);
        return JCE_TEXTURE_INVALID;
    }

    /* Decompress from PAK. */
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return JCE_TEXTURE_INVALID;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        JCE_FREE(buf);
        return JCE_TEXTURE_INVALID;
    }

    /* ── Cooked path: .jceasset → RGBA8 ── */
    if (jce_asset_is_cooked(buf, n)) {
        JceAssetView view;
        if (!jce_asset_open(&view, buf, n)) {
            LOG_ERROR(LOG_TAG, "bad .jceasset: %s", asset_path);
            JCE_FREE(buf);
            return JCE_TEXTURE_INVALID;
        }

        const JceAssetChunkEntry *info_chunk =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
        const JceAssetChunkEntry *pixel_chunk =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);

        if (!info_chunk || !pixel_chunk) {
            LOG_ERROR(LOG_TAG, "missing TEX_INFO or TEX_PIXELS: %s", asset_path);
            JCE_FREE(buf);
            return JCE_TEXTURE_INVALID;
        }

        /* Read texture info (info chunk may include mip offsets after the struct). */
        void *info_buf = JCE_MALLOC((size_t)info_chunk->original_size);
        if (!info_buf) { JCE_FREE(buf); return JCE_TEXTURE_INVALID; }

        if (jce_asset_chunk_data(&view, info_chunk,
                                  info_buf, (size_t)info_chunk->original_size) == 0) {
            JCE_FREE(info_buf);
            JCE_FREE(buf);
            return JCE_TEXTURE_INVALID;
        }

        JceAssetTexInfo tex_info;
        memcpy(&tex_info, info_buf, sizeof(tex_info));
        JCE_FREE(info_buf);

        /* Read RGBA8 pixel data. */
        void *tex_data = JCE_MALLOC((size_t)pixel_chunk->original_size);
        if (!tex_data) { JCE_FREE(buf); return JCE_TEXTURE_INVALID; }

        if (jce_asset_chunk_data(&view, pixel_chunk,
                                  tex_data,
                                  (size_t)pixel_chunk->original_size) == 0) {
            JCE_FREE(tex_data);
            JCE_FREE(buf);
            return JCE_TEXTURE_INVALID;
        }

        JCE_FREE(buf); /* PAK buffer no longer needed */

        /* Upload to bgfx as RGBA8. */
        const bgfx_memory_t *mem = bgfx_alloc((uint32_t)pixel_chunk->original_size);
        memcpy(mem->data, tex_data, pixel_chunk->original_size);
        JCE_FREE(tex_data);

        bool has_mips = tex_info.mip_count > 1;

        bgfx_texture_handle_t handle = bgfx_create_texture_2d(
            (uint16_t)tex_info.width, (uint16_t)tex_info.height,
            has_mips, 1, BGFX_TEXTURE_FORMAT_RGBA8,
            BGFX_TEXTURE_NONE | sampler_flags(sampler_mode), mem);

        if (handle.idx == UINT16_MAX)
            return JCE_TEXTURE_INVALID;

        registry_add(handle.idx, tex_info.width, tex_info.height);
        JceTexture tex;
        tex.idx = handle.idx;

        if (jce_texture_valid(tex)) {
            LOG_DEBUG(LOG_TAG, "loaded (cooked) %s [%ux%u, %u mips]",
                      asset_path, tex_info.width, tex_info.height,
                      tex_info.mip_count);
        }
        return tex;
    }

    /* ── Raw path: PNG/JPG → SDL3_image → RGBA8 ── */
    SDL_IOStream *io = SDL_IOFromConstMem(buf, (size_t)asset->original_size);
    if (!io) {
        JCE_FREE(buf);
        return JCE_TEXTURE_INVALID;
    }

    SDL_Surface *surf = IMG_Load_IO(io, true);  /* true = auto-close io */
    JCE_FREE(buf);

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

JceTexture jce_texture_load_ex(const JcePakArchive *pak, const char *asset_path,
                                int sampler_mode)
{
    JCE_PROFILE_ZONE_N("Texture::Load");
    JceTexture result = jce_texture_load_ex_inner(pak, asset_path, sampler_mode);
    JCE_PROFILE_ZONE_END;
    return result;
}

JceTexture jce_texture_from_rgba(const void *data,
                                  uint32_t width, uint32_t height)
{
    if (!data || width == 0 || height == 0)
        return JCE_TEXTURE_INVALID;

    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height,
        false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
        NULL);

    if (handle.idx == UINT16_MAX)
        return JCE_TEXTURE_INVALID;

    registry_add(handle.idx, width, height);

    const uint32_t bytes = width * height * 4u;
    const bgfx_memory_t *mem = bgfx_alloc(bytes);
    memcpy(mem->data, data, bytes);
    bgfx_update_texture_2d(handle,
                           0, /* layer */
                           0, /* mip */
                           0, /* x */
                           0, /* y */
                           (uint16_t)width,
                           (uint16_t)height,
                           mem,
                           (uint16_t)(width * 4u));

    JceTexture tex;
    tex.idx = handle.idx;
    return tex;
}

bool jce_texture_update_rgba(JceTexture tex, const void *data,
                             uint32_t width, uint32_t height)
{
    if (!jce_texture_valid(tex) || !data || width == 0 || height == 0)
        return false;

    /* If the registry has an entry, dimensions must match.  If the
     * registry overflowed (entry missing), trust the bgfx handle and
     * caller-provided dimensions — falling through to destroy+recreate
     * here would leak GPU memory each frame for high-throughput uploads
     * (e.g. video viewer) once MAX_TEXTURES is exceeded. */
    TexEntry *e = registry_find(tex.idx);
    if (e && (e->width != width || e->height != height))
        return false;

    const uint32_t bytes = width * height * 4u;
    const bgfx_memory_t *mem = bgfx_alloc(bytes);
    memcpy(mem->data, data, bytes);

    bgfx_texture_handle_t handle;
    handle.idx = tex.idx;
    bgfx_update_texture_2d(handle,
                           0, /* layer */
                           0, /* mip */
                           0, /* x */
                           0, /* y */
                           (uint16_t)width,
                           (uint16_t)height,
                           mem,
                           (uint16_t)(width * 4u));
    return true;
}

/* Zero-copy variant: bgfx takes a reference to caller-owned data.
 * Data must remain valid until bgfx_frame() is called (end of render frame).
 * Saves a ~33 MB memcpy per frame at 4K resolution vs jce_texture_update_rgba. */
bool jce_texture_update_rgba_ref(JceTexture tex, const void *data,
                                 uint32_t width, uint32_t height)
{
    if (!jce_texture_valid(tex) || !data || width == 0 || height == 0)
        return false;

    TexEntry *e = registry_find(tex.idx);
    if (e && (e->width != width || e->height != height))
        return false;

    const uint32_t bytes = width * height * 4u;
    const bgfx_memory_t *mem = bgfx_make_ref(data, bytes);

    bgfx_texture_handle_t handle;
    handle.idx = tex.idx;
    bgfx_update_texture_2d(handle,
                           0, /* layer */
                           0, /* mip */
                           0, /* x */
                           0, /* y */
                           (uint16_t)width,
                           (uint16_t)height,
                           mem,
                           (uint16_t)(width * 4u));
    return true;
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
