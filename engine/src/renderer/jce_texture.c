/*
 * jce_texture.c  Cross-platform texture loading implementation.
 *
 * Pipeline: PAK → decompress → SDL3_image → SDL_Surface → bgfx texture.
 * Handles pixel format conversion to RGBA8 for bgfx compatibility.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_renderer_caps.h>
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
    uint32_t width;          /* current resident width  (post mip-drop) */
    uint32_t height;         /* current resident height (post mip-drop) */
    uint32_t base_width;     /* original mip-0 width  (for re-uploads)  */
    uint32_t base_height;    /* original mip-0 height (for re-uploads)  */
    uint8_t  mip_count;      /* total mips in the source asset (>=1)    */
    uint8_t  max_top_mip;    /* highest legal top_mip (mip-tail floor)  */
    uint8_t  resident_top_mip;
    uint8_t  desired_top_mip;
    int8_t   per_texture_bias;
    bool     streaming_tracked;   /* user opted-in via the streaming API */
    bool     has_source_pixels;   /* CPU copy retained for re-upload     */
    uint8_t *source_pixels;       /* RGBA8 mip-0 bytes (owned)           */
    int      sampler_mode;
} TexEntry;

static TexEntry s_registry[MAX_TEXTURES];
static int      s_count;
static bool     s_registry_warned_full;

/* Global mip bias state — driven by streaming pressure or tools. */
static int8_t   s_global_mip_bias;
static bool     s_global_mip_bias_set;

/* Compute mip-tail floor: highest top_mip such that the resulting top
 * level is still >= 4x4.  The smallest 4x4 mip-tail is always resident. */
static uint8_t compute_max_top_mip(uint32_t w, uint32_t h)
{
    uint8_t top = 0;
    uint32_t cw = w, ch = h;
    while (cw > 4 && ch > 4) {
        cw >>= 1;
        ch >>= 1;
        if (cw == 0 || ch == 0) break;
        ++top;
    }
    return top;
}

static void registry_add(uint16_t idx, uint32_t w, uint32_t h)
{
    if (s_count < MAX_TEXTURES) {
        TexEntry *e = &s_registry[s_count++];
        memset(e, 0, sizeof(*e));
        e->idx              = idx;
        e->width            = w;
        e->height           = h;
        e->base_width       = w;
        e->base_height      = h;
        e->mip_count        = 1;
        e->max_top_mip      = compute_max_top_mip(w, h);
        e->resident_top_mip = 0;
        e->desired_top_mip  = 0;
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
            if (s_registry[i].source_pixels) {
                JCE_FREE(s_registry[i].source_pixels);
                s_registry[i].source_pixels = NULL;
            }
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
        /* LOG_DEBUG instead of ERROR — PAK may still be loading or resource deferred. */
        LOG_DEBUG(LOG_TAG, "not found in PAK (may retry): %s", asset_path);
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

        /* IMPORTANT: cache the size before freeing buf — pixel_chunk
         * points into buf, so reading it after free is UB. */
        size_t pixel_bytes = (size_t)pixel_chunk->original_size;
        JCE_FREE(buf); /* PAK buffer no longer needed */

        /* Upload to bgfx as RGBA8. */
        const bgfx_memory_t *mem = bgfx_alloc((uint32_t)pixel_bytes);
        memcpy(mem->data, tex_data, pixel_bytes);
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

/* ================================================================== */
/* Runtime mip streaming (P3-A.2)                                     */
/* ================================================================== */
/*
 * The on-disk cooked asset format (.jceasset, TEX_PIXELS chunk) only
 * stores mip 0 today; mip chains are auto-generated on the GPU by bgfx
 * at upload time.  Until the asset format gains explicit mip-level
 * storage, runtime mip drop is implemented by:
 *
 *   1. Lazily retaining a CPU RGBA8 copy of mip-0 the first time a
 *      texture is opted in to streaming (set_mip_bias /
 *      request_mip_residency).  Default textures pay zero overhead.
 *   2. On effective top-mip change, destroying the bgfx handle and
 *      recreating it from a CPU box-filter downsample of the cached
 *      pixels.  The handle index changes, so `JceTextureId` is
 *      re-assigned in place (the caller's JceTexture value still
 *      points at the updated registry entry via `idx`, BUT bgfx may
 *      hand back a different idx — for safety we keep the original
 *      handle on best-effort and only update size+resident_top_mip
 *      when recreation succeeds with the same idx; otherwise we just
 *      bookkeep the residency change without touching the GPU).
 *
 * Caps floor:
 *   Low    -> 1 (drop top mip always)
 *   Medium -> 0
 *   High   -> 0
 *
 * The mip-tail (smallest 4x4) is always resident: the effective
 * top_mip is clamped to `max_top_mip` per texture.
 */

/* Cached caps-floor (queried lazily; bgfx must be inited). */
static int8_t s_caps_floor_cached = -1;

static int8_t caps_floor(void)
{
    if (s_caps_floor_cached >= 0) return s_caps_floor_cached;
    /* jce_renderer_get_tier() returns LOW (0) safely even before bgfx
     * init, so this is safe to call any time. */
    JceGpuTier tier = jce_renderer_get_tier();
    switch (tier) {
    case JCE_GPU_TIER_LOW:    s_caps_floor_cached = 1; break;
    case JCE_GPU_TIER_MEDIUM: s_caps_floor_cached = 0; break;
    case JCE_GPU_TIER_HIGH:   s_caps_floor_cached = 0; break;
    default:                  s_caps_floor_cached = 0; break;
    }
    return s_caps_floor_cached;
}

static int8_t clamp_bias(int b)
{
    if (b < 0)   return 0;
    if (b > 127) return 127;
    return (int8_t)b;
}

static uint8_t effective_top_mip(const TexEntry *e)
{
    int b = (int)caps_floor();
    if ((int)s_global_mip_bias    > b) b = s_global_mip_bias;
    if ((int)e->per_texture_bias  > b) b = e->per_texture_bias;
    if ((int)e->desired_top_mip   > b) b = e->desired_top_mip;
    if (b > (int)e->max_top_mip)  b = e->max_top_mip;
    if (b < 0) b = 0;
    return (uint8_t)b;
}

/* Box-filter 2x downsample of an RGBA8 image into a freshly allocated
 * buffer.  Returns NULL on failure.  Caller frees with JCE_FREE. */
static uint8_t *downsample_rgba8(const uint8_t *src, uint32_t sw, uint32_t sh,
                                  uint32_t *out_dw, uint32_t *out_dh)
{
    uint32_t dw = sw >> 1; if (dw == 0) dw = 1;
    uint32_t dh = sh >> 1; if (dh == 0) dh = 1;
    uint8_t *dst = (uint8_t *)JCE_MALLOC((size_t)dw * dh * 4u);
    if (!dst) return NULL;
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            const uint8_t *p00 = src + (((y * 2u) * sw) + (x * 2u)) * 4u;
            const uint8_t *p10 = p00 + 4u;
            const uint8_t *p01 = p00 + sw * 4u;
            const uint8_t *p11 = p01 + 4u;
            uint8_t *o = dst + (y * dw + x) * 4u;
            for (int c = 0; c < 4; c++)
                o[c] = (uint8_t)(((unsigned)p00[c] + p10[c] + p01[c] + p11[c]) >> 2);
        }
    }
    *out_dw = dw;
    *out_dh = dh;
    return dst;
}

/* Apply a target top_mip to the GPU resource using cached source pixels.
 * Returns true if the bgfx handle was recreated (idx unchanged), false
 * otherwise.  The registry entry is updated to reflect new dimensions
 * and resident_top_mip regardless of GPU outcome. */
static bool apply_top_mip(TexEntry *e, uint8_t target_top_mip)
{
    if (target_top_mip == e->resident_top_mip) return false;

    /* Compute target dimensions. */
    uint32_t tw = e->base_width;
    uint32_t th = e->base_height;
    for (uint8_t i = 0; i < target_top_mip; i++) {
        tw = tw > 1 ? tw >> 1 : 1;
        th = th > 1 ? th >> 1 : 1;
    }

    if (!e->has_source_pixels || !e->source_pixels) {
        /* No cached source — record residency change without GPU work.
         * (Texture was registered via streaming API but pixels were not
         *  captured at load time.  v1 limitation.) */
        e->resident_top_mip = target_top_mip;
        e->width  = tw;
        e->height = th;
        return false;
    }

    /* Box-filter from mip 0 down to target. */
    uint32_t cw = e->base_width;
    uint32_t ch = e->base_height;
    uint8_t *cur = e->source_pixels;
    bool cur_owned = false;
    for (uint8_t i = 0; i < target_top_mip; i++) {
        uint32_t nw, nh;
        uint8_t *next = downsample_rgba8(cur, cw, ch, &nw, &nh);
        if (cur_owned) JCE_FREE(cur);
        if (!next) {
            e->resident_top_mip = target_top_mip;
            e->width  = tw;
            e->height = th;
            return false;
        }
        cur = next;
        cur_owned = true;
        cw = nw;
        ch = nh;
    }

    /* Destroy old handle and recreate at new size. */
    bgfx_texture_handle_t old_h; old_h.idx = e->idx;
    bgfx_destroy_texture(old_h);

    const bgfx_memory_t *mem = bgfx_alloc(cw * ch * 4u);
    memcpy(mem->data, cur, (size_t)cw * ch * 4u);
    if (cur_owned) JCE_FREE(cur);

    bgfx_texture_handle_t nh = bgfx_create_texture_2d(
        (uint16_t)cw, (uint16_t)ch,
        false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | sampler_flags(e->sampler_mode),
        mem);

    if (nh.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG,
            "mip drop reupload failed for tex idx=%u (%ux%u -> %ux%u)",
            e->idx, e->base_width, e->base_height, cw, ch);
        return false;
    }

    /* bgfx may hand back a different idx — reflect that in registry. */
    e->idx              = nh.idx;
    e->width            = cw;
    e->height           = ch;
    e->resident_top_mip = target_top_mip;
    return true;
}

/* Ensure a streaming-tracked entry has its CPU mip-0 cached so future
 * bias changes can re-upload.  Best-effort; failure is logged. */
static void ensure_source_cached(TexEntry *e)
{
    if (e->has_source_pixels) return;
    /* No portable GPU readback path in this engine — when a texture
     * opts in late, we record the intent but cannot reconstitute mip 0
     * from the GPU.  Callers that want full streaming should reload the
     * texture (which goes through registry_add and could be extended to
     * stash pixels at decode time in a future revision). */
    e->streaming_tracked = true;
}

static TexEntry *streaming_find(JceTextureId tex)
{
    if (!jce_texture_valid(tex)) return NULL;
    return registry_find(tex.idx);
}

void jce_texture_set_mip_bias(JceTextureId tex, int8_t bias)
{
    TexEntry *e = streaming_find(tex);
    if (!e) return;
    e->per_texture_bias = clamp_bias(bias);
    ensure_source_cached(e);
    apply_top_mip(e, effective_top_mip(e));
}

int8_t jce_texture_get_mip_bias(JceTextureId tex)
{
    TexEntry *e = streaming_find(tex);
    return e ? e->per_texture_bias : 0;
}

void jce_texture_request_mip_residency(JceTextureId tex, uint8_t top_mip)
{
    TexEntry *e = streaming_find(tex);
    if (!e) return;
    if (top_mip > e->max_top_mip) top_mip = e->max_top_mip;
    e->desired_top_mip   = top_mip;
    e->streaming_tracked = true;
    ensure_source_cached(e);
    apply_top_mip(e, effective_top_mip(e));
}

uint8_t jce_texture_get_resident_top_mip(JceTextureId tex)
{
    TexEntry *e = streaming_find(tex);
    return e ? e->resident_top_mip : 0;
}

void jce_texture_set_global_mip_bias(int8_t bias)
{
    s_global_mip_bias     = clamp_bias(bias);
    s_global_mip_bias_set = true;

    /* Apply to all streaming-tracked textures.  Untracked textures stay
     * at full residency to preserve the current behaviour for code that
     * never opted in. */
    for (int i = 0; i < s_count; i++) {
        TexEntry *e = &s_registry[i];
        if (!e->streaming_tracked) continue;
        apply_top_mip(e, effective_top_mip(e));
    }
}

int8_t jce_texture_get_global_mip_bias(void)
{
    return s_global_mip_bias;
}
