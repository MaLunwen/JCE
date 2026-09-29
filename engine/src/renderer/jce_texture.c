/*
 * jce_texture.c  Cross-platform texture loading implementation.
 *
 * Pipeline: PAK → decompress → jce_image decode → RGBA8 → bgfx texture.
 * The decode itself belongs to the jce_image service — this file never
 * picks a codec.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>    /* parallel mip downsample */
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_image.h>    /* the one image-decode service */
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_asset_format.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_filesystem.h> /* jce_fs_host_read_all (LUT host loader) */

#include "jce_texture_internal.h"      /* jce_lut_strip_to_volume decl */
#include "os/core/jce_memory.h"
#include "resource/jce_asset_reader.h"
#include "resource/jce_tex_compress.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>   /* SDL_Surface: the pre-decoded upload entry point */
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
    uint8_t  bgfx_fmt;            /* bgfx_texture_format_t at creation
                                     (0xFF = unknown / never batchable)  */
} TexEntry;

static TexEntry s_registry[MAX_TEXTURES];
static int      s_count;
static bool     s_registry_warned_full;

/* Global mip bias state — driven by streaming pressure or tools. */
static int8_t   s_global_mip_bias;
static bool     s_global_mip_bias_set;
/* Project Settings > Quality > Texture Quality (Full/Half/Quarter/Eighth ->
 * 0/1/2/3): a BASE mip-drop that stacks (max) with streaming pressure. */
static int8_t   s_texture_quality_bias;
/* Project Settings > Graphics > Anisotropic Textures override: <0 keeps the
 * tier default (aniso on HIGH+); 0 forces off; >0 forces on. */
static int8_t   s_aniso_override = -1;
/* Project Settings > Graphics > Color Space.  1 = linear (hardware sRGB decode
 * on the textures that ask, and an encoded frame on the way out), 0 = gamma
 * (neither).  Default linear: it is what this engine has always done and what
 * every comparable engine does. */
static int      s_colour_space_linear = 1;

/* VRAM ceiling (large-world-opt): when set, NEW raw-RGBA8 texture uploads
 * (the streamed-texture path: PAK PNG/JPG → SDL_Surface, and cooked RGBA8)
 * retain a CPU mip-0 copy AND opt into streaming_tracked, so the existing
 * streaming-pressure → global-mip-bias hook can PHYSICALLY drop their top mips
 * under memory pressure (without a retained source the demote is a truthful
 * no-op — audit F29).  Off by default so editor/UI/one-off textures pay zero
 * extra RAM; the runtime renderer arms it around streamed uploads.  Process-
 * wide + render-thread-set (uploads happen on the render thread). */
static bool     s_streaming_uploads;

void jce_texture_set_streaming_uploads(bool on)
{
    s_streaming_uploads = on;
}

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

/* Full mip-chain level count for a WxH texture (floor(log2(max))+1). */
static uint8_t full_mip_count(uint32_t w, uint32_t h)
{
    uint32_t m = (w > h) ? w : h, n = 1u;
    while (m > 1u) { m >>= 1; n++; }
    return (uint8_t)(n > 255u ? 255u : n);
}

/* O(1) bgfx-handle-idx -> registry-slot map (value slot+1, 0 = none).
 * bgfx texture handle indices are DENSE allocator values below
 * BGFX_CONFIG_MAX_TEXTURES, so a flat array beats any hash: no probing,
 * no deletion tombstones, and bgfx handle reuse after destroy is naturally
 * correct (remove clears the cell, the next add rewrites it).
 * registry_find was a linear scan over up to 4096 entries per call — it
 * sits on the get_size / mip-bias / streaming-demote query paths.
 * Indices beyond the flat cap (a raised bgfx config) fall back to the old
 * linear scan — correctness never depends on the map. */
#define REG_IDX_CAP 8192u   /* 2x bgfx default MAX_TEXTURES, 16KB static */
static uint16_t s_reg_slot[REG_IDX_CAP];

static void registry_add(uint16_t idx, uint32_t w, uint32_t h, uint8_t mip_count,
                         uint8_t bgfx_fmt)
{
    if (s_count < MAX_TEXTURES) {
        TexEntry *e = &s_registry[s_count++];
        memset(e, 0, sizeof(*e));
        e->idx              = idx;
        e->width            = w;
        e->height           = h;
        e->base_width       = w;
        e->base_height      = h;
        e->mip_count        = mip_count < 1u ? 1u : mip_count;
        e->max_top_mip      = compute_max_top_mip(w, h);
        e->resident_top_mip = 0;
        e->desired_top_mip  = 0;
        e->bgfx_fmt         = bgfx_fmt;
        if (idx < REG_IDX_CAP)
            s_reg_slot[idx] = (uint16_t)s_count;   /* slot+1 */
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
    if (idx < REG_IDX_CAP) {
        uint16_t s = s_reg_slot[idx];
        if (!s) return NULL;
        TexEntry *e = &s_registry[s - 1];
        /* The slot record is the source of truth (paranoia against a stale
         * cell); heals nothing — add/remove keep the map exact. */
        return (e->idx == idx) ? e : NULL;
    }
    for (int i = 0; i < s_count; i++)                 /* flat-cap overflow */
        if (s_registry[i].idx == idx)
            return &s_registry[i];
    return NULL;
}

static void registry_remove(uint16_t idx)
{
    TexEntry *e = registry_find(idx);
    if (!e) return;
    if (e->source_pixels) {
        JCE_FREE(e->source_pixels);
        e->source_pixels = NULL;
    }
    if (idx < REG_IDX_CAP)
        s_reg_slot[idx] = 0;
    int i = (int)(e - s_registry);
    s_registry[i] = s_registry[--s_count];            /* swap-remove */
    /* The moved (former last) entry changed slots — remap it. */
    if (i < s_count && s_registry[i].idx < REG_IDX_CAP)
        s_reg_slot[s_registry[i].idx] = (uint16_t)(i + 1);
}

/* VRAM ceiling: opt a freshly-registered texture into streaming + retain a CPU
 * RGBA8 mip-0 copy so the streaming-pressure mip-bias hook can physically shrink
 * it.  Called only when s_streaming_uploads is armed (runtime streamed uploads).
 * `rgba8` is the tightly-packed (w*h*4) mip-0; a private copy is taken. */
static void registry_opt_in_streaming(uint16_t idx, uint32_t w, uint32_t h,
                                      const void *rgba8, int sampler_mode)
{
    if (!s_streaming_uploads || !rgba8 || w == 0 || h == 0) return;
    TexEntry *e = registry_find(idx);
    if (!e || e->has_source_pixels) return;
    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *copy = (uint8_t *)JCE_MALLOC(bytes);
    if (!copy) return;   /* best-effort; without it the demote stays a no-op */
    memcpy(copy, rgba8, bytes);
    e->source_pixels     = copy;
    e->has_source_pixels = true;
    e->streaming_tracked = true;
    e->sampler_mode      = sampler_mode;
}

/* -- Helpers -------------------------------------------------------- */

/* Map a cooked JCEASSET_TEXFMT_* GPU format to its bgfx texture format.
 * Block-compressed formats (BC/ASTC/ETC2) are uploaded as-is — the GPU
 * samples them directly with no runtime decode. Anything unrecognized
 * (including RGBA8/RGB8) falls back to RGBA8. Shared by the synchronous
 * cooked loader and the async finalize path so both honor the cooked
 * format identically. */
static bgfx_texture_format_t texfmt_to_bgfx(uint32_t fmt)
{
    switch (fmt) {
    case JCEASSET_TEXFMT_RGBA8:      return BGFX_TEXTURE_FORMAT_RGBA8;
    case JCEASSET_TEXFMT_RGB8:       return BGFX_TEXTURE_FORMAT_RGB8;
    case JCEASSET_TEXFMT_BC1:        return BGFX_TEXTURE_FORMAT_BC1;
    case JCEASSET_TEXFMT_BC3:        return BGFX_TEXTURE_FORMAT_BC3;
    case JCEASSET_TEXFMT_BC5:        return BGFX_TEXTURE_FORMAT_BC5;
    case JCEASSET_TEXFMT_BC7:        return BGFX_TEXTURE_FORMAT_BC7;
    case JCEASSET_TEXFMT_ASTC_4x4:   return BGFX_TEXTURE_FORMAT_ASTC4X4;
    case JCEASSET_TEXFMT_ETC2_RGBA8: return BGFX_TEXTURE_FORMAT_ETC2A;
    case JCEASSET_TEXFMT_R16F:       return BGFX_TEXTURE_FORMAT_R16F;
    case JCEASSET_TEXFMT_RG16F:      return BGFX_TEXTURE_FORMAT_RG16F;
    case JCEASSET_TEXFMT_RGBA16F:    return BGFX_TEXTURE_FORMAT_RGBA16F;
    case JCEASSET_TEXFMT_R32F:       return BGFX_TEXTURE_FORMAT_R32F;
    case JCEASSET_TEXFMT_RG32F:      return BGFX_TEXTURE_FORMAT_RG32F;
    case JCEASSET_TEXFMT_RGBA32F:    return BGFX_TEXTURE_FORMAT_RGBA32F;
    default:                         return BGFX_TEXTURE_FORMAT_COUNT;
    }
}

/* Map sampler mode to bgfx flags. */
static uint64_t sampler_flags(int mode)
{
    /* Anisotropic filtering on capable HW only (HIGH tier) — the Unity/UE norm
     * for grazing-angle quality, and only meaningful now that runtime textures
     * carry a mip chain.  The charter weak-GPU baseline (LOW/MED) stays plain
     * trilinear (no per-sample aniso cost — multi-tap filtering is the kind of
     * opt-in extra the charter reserves for capable devices).  UI/sprite (CLAMP)
     * textures are sampled ~1:1 and never benefit, so they skip it. */
    uint64_t aniso = 0;
    /* Project Settings > Graphics > Anisotropic Textures overrides the tier
     * default: <0 = tier gate (HIGH+); 0 = force off; >0 = force on. */
    const bool want_aniso = (s_aniso_override < 0)
        ? (jce_renderer_get_tier() >= JCE_GPU_TIER_HIGH)
        : (s_aniso_override > 0);
    if (want_aniso)
        aniso = BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
    /* sRGB is a bit on the same argument, orthogonal to the address mode --
     * so it is stripped before the switch and OR'd back onto every arm. */
    /* GAMMA mode decodes nothing: the request is honoured only in linear. */
    const uint64_t srgb = (s_colour_space_linear && (mode & JCE_TEX_SRGB))
                        ? BGFX_TEXTURE_SRGB : 0u;
    mode &= ~JCE_TEX_SRGB;
    switch (mode) {
    case JCE_TEX_WRAP:
        return aniso | srgb; /* default wrap behavior */
    case JCE_TEX_MIRROR:
        return BGFX_SAMPLER_U_MIRROR | BGFX_SAMPLER_V_MIRROR | aniso | srgb;
    default: /* JCE_TEX_CLAMP (UI/sprite — no aniso) */
        return BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | srgb;
    }
}

/* Does this format have an sRGB view ON THIS DEVICE?
 *
 * Asked of the driver rather than answered from a table.  BGFX_TEXTURE_SRGB
 * on a format without an sRGB view is not an error and not a conversion -- it
 * is ignored, and the caller gets an encoded value it has stopped decoding.
 * A hand-written whitelist would have to be right about every backend and
 * every format bgfx adds later; the caps bit is the same question asked of
 * the thing that knows. */
static bool format_has_srgb(bgfx_texture_format_t f)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps) return false;
    return (caps->formats[f] & BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB) != 0u;
}


/* Create a bgfx texture from an RGBA8 surface. */
static uint8_t *downsample_rgba8(const uint8_t *src, uint32_t sw, uint32_t sh,
                                 uint32_t *out_dw, uint32_t *out_dh); /* fwd (mip chain) */

/* ── Mip-chain blob (shared by every RGBA8 upload path) ─────────────
 *
 * Split in two at the seam where the callers differ: each one packs mip 0 its
 * own way (a tier-capped copy, a tight memcpy, or a row walk over a padded
 * pitch), and every one of them then wants the same box-filtered rest.  Kept as
 * two functions rather than one so a caller cannot accidentally get a chain
 * whose level 0 was generated from uninitialised memory. */

/* Sized for the FULL chain; only the mip-0 region is left for the caller. */
static const bgfx_memory_t *alloc_mip_chain_rgba8(uint32_t w, uint32_t h)
{
    size_t total = 0;
    for (uint32_t mw = w, mh = h;;) {
        total += (size_t)mw * mh * 4u;
        if (mw <= 1u && mh <= 1u) break;
        mw = mw > 1u ? mw >> 1 : 1u; mh = mh > 1u ? mh >> 1 : 1u;
    }
    return bgfx_alloc((uint32_t)total);
}

/* Levels 1..N-1, box-filtered in place.  Mip 0 must already be written. */
static void fill_mip_chain_rgba8(const bgfx_memory_t *mem, uint32_t w, uint32_t h)
{
    if (!mem) return;
    uint8_t *prev = mem->data; uint32_t pw = w, ph = h;
    uint8_t *cur  = mem->data + (size_t)w * h * 4u;
    while (pw > 1u || ph > 1u) {
        const uint32_t nw = pw > 1u ? pw >> 1 : 1u;
        const uint32_t nh = ph > 1u ? ph >> 1 : 1u;
        uint32_t gw = 0, gh = 0;
        uint8_t *ds = downsample_rgba8(prev, pw, ph, &gw, &gh);
        /* On OOM: a black level is wrong but bounded; leaving it uninitialised
         * would upload whatever the allocator last held, which reads as random
         * coloured confetti at distance and gets blamed on the texture. */
        if (ds) { memcpy(cur, ds, (size_t)nw * nh * 4u); JCE_FREE(ds); }
        else    { memset(cur, 0, (size_t)nw * nh * 4u); }
        prev = cur; pw = nw; ph = nh; cur += (size_t)nw * nh * 4u;
    }
}

/* Core upload: RGBA8 rows at `pitch` bytes.  Both entry points (the decoded
 * jce_image buffer and the pre-decoded SDL_Surface handed in by the async
 * pool) funnel here so the tier cap / mip chain / streaming opt-in run
 * identically whatever produced the pixels. */
static JceTexture texture_from_rgba8_ex(const uint8_t *src_pixels, uint32_t w, uint32_t h,
                                        uint32_t pitch, int mode)
{
    if (!src_pixels) return JCE_TEXTURE_INVALID;

    uint32_t expected_pitch = w * 4;

    /* Charter tier cap (512MB / weak-GPU baseline): runtime-uploaded SCENE
     * textures (WRAP/MIRROR) obey the tier's max_texture_size the same way
     * cooked assets already do at cook time (jce_asset_cooker) — before this,
     * a raw 4K PNG uploaded full-size + full mip chain on the LOW tier, where
     * the recommendation is 1024.  UI/sprite/LUT textures (CLAMP) are sampled
     * ~1:1 and are exempt (shrinking them visibly blurs the editor UI).  The
     * box-filter shrink reuses the mip downsampler; HIGH/ULTRA recommend
     * 2048/4096 so strong machines only ever clamp pathological sources.
     * `shrunk` is tightly packed; the mip-0 copy below consumes it in place
     * of the surface and frees it after the bgfx blob is filled. */
    uint8_t *shrunk = NULL;
    if (mode != JCE_TEX_CLAMP) {
        JceRenderRecommendation lrec = jce_renderer_get_recommendation();
        uint32_t cap = lrec.max_texture_size;
        if (cap >= 256u && (w > cap || h > cap)) {
            /* Tightly pack the source once (downsample_rgba8 expects packed). */
            uint8_t *packed = (uint8_t *)JCE_MALLOC((size_t)w * h * 4u);
            if (packed) {
                if (pitch == expected_pitch) {
                    memcpy(packed, src_pixels, (size_t)w * h * 4u);
                } else {
                    const uint8_t *src = src_pixels;
                    for (uint32_t y = 0; y < h; y++)
                        memcpy(packed + (size_t)y * expected_pitch,
                               src + (size_t)y * pitch, expected_pitch);
                }
                uint32_t cw = w, ch = h;
                uint8_t *cur = packed;
                while (cw > cap || ch > cap) {
                    uint32_t dw = 0, dh = 0;
                    uint8_t *ds = downsample_rgba8(cur, cw, ch, &dw, &dh);
                    if (!ds) break;          /* OOM: upload what we have */
                    JCE_FREE(cur);
                    cur = ds; cw = dw; ch = dh;
                }
                shrunk = cur;
                w = cw; h = ch;
                pitch = expected_pitch = w * 4u;
            }
        }
    }

    /* Generate a full box-filtered mip chain so minified textures filter
     * trilinearly instead of aliasing/shimmering — the Unity/UE norm, and exactly
     * what the cooked-asset path (jce_texture_from_cooked) already does.  The raw
     * runtime path used to upload mip-0 only.  Levels are concatenated into one
     * blob the way bgfx expects for a mipped create; the existing
     * downsample_rgba8 produces each 2x level.  +33% VRAM is reclaimable by the
     * streaming-pressure mip-bias hook (registry_opt_in_streaming below). */
    const bgfx_memory_t *mem = alloc_mip_chain_rgba8(w, h);
    /* mip 0: tightly-packed copy (row-by-row if the surface pitch is padded).
     * When the tier cap shrunk the source above, `shrunk` IS the packed mip-0. */
    if (shrunk) {
        memcpy(mem->data, shrunk, (size_t)w * h * 4u);
        JCE_FREE(shrunk);
        shrunk = NULL;
    } else if (pitch == expected_pitch) {
        memcpy(mem->data, src_pixels, (size_t)w * h * 4u);
    } else {
        const uint8_t *src = src_pixels;
        uint8_t *dst = mem->data;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(dst, src, expected_pitch);
            src += pitch;
            dst += expected_pitch;
        }
    }
    fill_mip_chain_rgba8(mem, w, h);

    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)w, (uint16_t)h,
        true,   /* mip chain (box-filtered above) → trilinear minification */
        1,      /* layers */
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | sampler_flags(mode),
        mem, 0);

    if (handle.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "bgfx_create_texture_2d failed (%ux%u)", w, h);
        return JCE_TEXTURE_INVALID;
    }

    registry_add(handle.idx, w, h, full_mip_count(w, h),
                 (uint8_t)BGFX_TEXTURE_FORMAT_RGBA8);
    /* VRAM ceiling: streamed RGBA8 uploads retain a CPU mip-0 + opt into
     * streaming so mip-bias under pressure physically reclaims VRAM.  mem->data
     * is the tightly-packed RGBA8 we just filled (valid this frame, same
     * thread).  No-op unless streaming uploads are armed. */
    registry_opt_in_streaming(handle.idx, w, h, mem->data, mode);

    JceTexture tex;
    tex.idx = handle.idx;
    return tex;
}

/* -- Public API ----------------------------------------------------- */

JceTexture jce_texture_load_from_surface(const void *surface, int sampler_mode)
{
    if (!surface) return JCE_TEXTURE_INVALID;
    const SDL_Surface *surf = (const SDL_Surface *)surface;
    return texture_from_rgba8_ex((const uint8_t *)surf->pixels, (uint32_t)surf->w,
                                 (uint32_t)surf->h, (uint32_t)surf->pitch, sampler_mode);
}

JceTexture jce_texture_load(const JcePakArchive *pak, const char *asset_path)
{
    return jce_texture_load_ex(pak, asset_path, JCE_TEX_CLAMP);
}

/* CPU-side decode result (see jce_texture.h).  Holds EITHER a cooked
 * payload (is_cooked: info + concatenated mip pixels) OR a raw, tightly
 * packed RGBA8 buffer from the jce_image service.  Owns its buffers;
 * upload_cpu/cpu_free release them (each with its own allocator). */
struct JceTextureCpu {
    int             sampler_mode;
    bool            is_cooked;
    JceAssetTexInfo info;          /* cooked */
    void           *pixels;        /* cooked: owned TEX_PIXELS payload */
    size_t          pixel_bytes;
    uint8_t        *rgba8;         /* raw: owned RGBA8, jce_image_free_rgba8 */
    uint32_t        rgba8_w;
    uint32_t        rgba8_h;
};

JceTextureCpu *jce_texture_decode_cpu(const JcePakArchive *pak,
                                      const char *asset_path,
                                      int sampler_mode)
{
    if (!pak || !asset_path) return NULL;

    /* Extension whitelist — silently reject obvious non-image assets.
     * Some scenes accidentally point texture fields at .obj / .glb /
     * .fbx files; the asset manager finds them in the PAK and feeds the
     * bytes to the decoder which then spams ERROR every frame. Filtering by
     * extension keeps the log clean and short-circuits the wasted work. */
    {
        const char *dot = strrchr(asset_path, '.');
        if (dot) {
            static const char *exts[] = {
                ".png", ".jpg", ".jpeg", ".tga", ".dds", ".ktx",
                ".ktx2", ".bmp", ".hdr", ".webp", ".psd", ".gif",
                ".jceasset", NULL
            };
            bool ok = false;
            for (int i = 0; exts[i] && !ok; ++i) {
                if (jce_strcasecmp(dot, exts[i]) == 0) ok = true;
            }
            if (!ok) {
                LOG_DEBUG(LOG_TAG, "skipping non-image asset: %s", asset_path);
                return NULL;
            }
        }
    }

    const JcePakAsset *asset = jce_pak_find(pak, asset_path);
    if (!asset) {
        /* LOG_DEBUG instead of ERROR — PAK may still be loading or resource deferred. */
        LOG_DEBUG(LOG_TAG, "not found in PAK (may retry): %s", asset_path);
        return NULL;
    }

    /* Decompress from PAK. */
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        JCE_FREE(buf);
        return NULL;
    }

    /* ── Cooked path: .jceasset ── */
    if (jce_asset_is_cooked(buf, n)) {
        JceAssetView view;
        if (!jce_asset_open(&view, buf, n)) {
            LOG_ERROR(LOG_TAG, "bad .jceasset: %s", asset_path);
            JCE_FREE(buf);
            return NULL;
        }

        const JceAssetChunkEntry *info_chunk =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
        const JceAssetChunkEntry *pixel_chunk =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);

        if (!info_chunk || !pixel_chunk) {
            LOG_ERROR(LOG_TAG, "missing TEX_INFO or TEX_PIXELS: %s", asset_path);
            JCE_FREE(buf);
            return NULL;
        }

        /* Read texture info (info chunk may include mip offsets after the struct). */
        void *info_buf = JCE_MALLOC((size_t)info_chunk->original_size);
        if (!info_buf) { JCE_FREE(buf); return NULL; }

        if (jce_asset_chunk_data(&view, info_chunk,
                                  info_buf, (size_t)info_chunk->original_size) == 0) {
            JCE_FREE(info_buf);
            JCE_FREE(buf);
            return NULL;
        }

        JceAssetTexInfo tex_info;
        memcpy(&tex_info, info_buf, sizeof(tex_info));
        JCE_FREE(info_buf);

        /* Read pixel data (RGBA8 or block-compressed mip chain). */
        size_t pixel_bytes = (size_t)pixel_chunk->original_size;
        void *tex_data = JCE_MALLOC(pixel_bytes);
        if (!tex_data) { JCE_FREE(buf); return NULL; }

        if (jce_asset_chunk_data(&view, pixel_chunk,
                                  tex_data, pixel_bytes) == 0) {
            JCE_FREE(tex_data);
            JCE_FREE(buf);
            return NULL;
        }
        JCE_FREE(buf); /* PAK buffer no longer needed */

        JceTextureCpu *c = JCE_MALLOC(sizeof(*c));
        if (!c) { JCE_FREE(tex_data); return NULL; }
        memset(c, 0, sizeof(*c));
        c->sampler_mode = sampler_mode;
        c->is_cooked    = true;
        c->info         = tex_info;
        c->pixels       = tex_data;
        c->pixel_bytes  = pixel_bytes;
        return c;
    }

    /* ── Raw path: PNG/JPG → jce_image → RGBA8 ── */
    int img_w = 0, img_h = 0;
    uint8_t *rgba8 = jce_image_load_rgba8_from_memory(buf, (uint64_t)asset->original_size,
                                                      &img_w, &img_h);
    JCE_FREE(buf);

    if (!rgba8) {
        /* The codec-level reason is logged by the image service itself. */
        LOG_ERROR(LOG_TAG, "image decode failed for %s", asset_path);
        return NULL;
    }

    JceTextureCpu *c = JCE_MALLOC(sizeof(*c));
    if (!c) { jce_image_free_rgba8(rgba8); return NULL; }
    memset(c, 0, sizeof(*c));
    c->sampler_mode = sampler_mode;
    c->is_cooked    = false;
    c->rgba8        = rgba8;
    c->rgba8_w      = (uint32_t)img_w;
    c->rgba8_h      = (uint32_t)img_h;
    return c;
}

JceTextureCpu *jce_texture_decode_cpu_mem(const void *encoded, size_t size,
                                          int sampler_mode)
{
    if (!encoded || size == 0) return NULL;
    int img_w = 0, img_h = 0;
    uint8_t *rgba8 = jce_image_load_rgba8_from_memory(encoded, (uint64_t)size,
                                                      &img_w, &img_h);
    if (!rgba8) return NULL;

    JceTextureCpu *c = JCE_MALLOC(sizeof(*c));
    if (!c) { jce_image_free_rgba8(rgba8); return NULL; }
    memset(c, 0, sizeof(*c));
    c->sampler_mode = sampler_mode;
    c->is_cooked    = false;
    c->rgba8        = rgba8;
    c->rgba8_w      = (uint32_t)img_w;
    c->rgba8_h      = (uint32_t)img_h;
    return c;
}

bool jce_texture_decode_cooked_rgba8(const void *encoded, size_t size,
                                     uint8_t **out_rgba8,
                                     uint32_t *out_w, uint32_t *out_h)
{
    if (out_rgba8) *out_rgba8 = NULL;
    if (!encoded || size == 0 || !out_rgba8 || !out_w || !out_h)
        return false;
    if (!jce_asset_is_cooked(encoded, size))
        return false;

    JceAssetView view;
    if (!jce_asset_open(&view, encoded, size))
        return false;
    const JceAssetChunkEntry *info_c =
        jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
    const JceAssetChunkEntry *pix_c =
        jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);
    if (!info_c || !pix_c)
        return false;

    JceAssetTexInfo info;
    if (jce_asset_chunk_data(&view, info_c, &info, sizeof(info)) < sizeof(info))
        return false;
    if (info.width == 0 || info.height == 0)
        return false;

    size_t pix_bytes = (size_t)pix_c->original_size;
    void *pixels = JCE_MALLOC(pix_bytes);
    if (!pixels)
        return false;
    size_t pix_copied = jce_asset_chunk_data(&view, pix_c, pixels, pix_bytes);
    if (pix_copied == 0) {
        JCE_FREE(pixels);
        return false;
    }

    /* Reject a pixel chunk too small for the declared base mip: the decode
     * below reads width*height blocks, so a short chunk reads OOB.  Validate
     * against the bytes ACTUALLY produced (decompressed size, or the clamped
     * raw copy) rather than the declared original_size — a chunk that claims a
     * large original_size but supplies fewer real bytes would otherwise decode
     * the base mip from uninitialized heap (audit Round-3 P3). */
    uint32_t base_need = jce_tex_cooked_pixel_size(info.width, info.height,
                                                   (int)info.format, 1);
    if (base_need == 0 || pix_copied < base_need) {
        JCE_FREE(pixels);
        return false;
    }

    size_t rgba_bytes = (size_t)info.width * (size_t)info.height * 4u;
    uint8_t *rgba = (uint8_t *)JCE_MALLOC(rgba_bytes);
    if (!rgba) { JCE_FREE(pixels); return false; }

    /* The base mip is at offset 0 of the pixel chunk; jce_tex_decode_to_rgba8
     * reads only that mip's blocks. */
    int ok = jce_tex_decode_to_rgba8(pixels, info.width, info.height,
                                     (int)info.format, rgba);
    JCE_FREE(pixels);
    if (!ok) { JCE_FREE(rgba); return false; }

    *out_rgba8 = rgba;
    *out_w = info.width;
    *out_h = info.height;
    return true;
}

JceTexture jce_texture_upload_cpu(JceTextureCpu *c)
{
    if (!c) return JCE_TEXTURE_INVALID;
    JceTexture tex = JCE_TEXTURE_INVALID;
    if (c->is_cooked) {
        tex = jce_texture_from_cooked(&c->info, c->pixels, c->pixel_bytes,
                                      c->sampler_mode);
    } else if (c->rgba8) {
        tex = texture_from_rgba8_ex(c->rgba8, c->rgba8_w, c->rgba8_h,
                                    c->rgba8_w * 4u, c->sampler_mode);
    }
    jce_texture_cpu_free(c);
    return tex;
}

void jce_texture_cpu_free(JceTextureCpu *c)
{
    if (!c) return;
    /* Each buffer goes back to the allocator that produced it: the cooked
     * payload is ours, the raw pixels belong to the image service. */
    if (c->pixels) JCE_FREE(c->pixels);
    if (c->rgba8)  jce_image_free_rgba8(c->rgba8);
    JCE_FREE(c);
}

JceTexture jce_texture_load_ex(const JcePakArchive *pak, const char *asset_path,
                                int sampler_mode)
{
    JCE_PROFILE_ZONE_N("Texture::Load");
    /* Synchronous = decode (worker-safe) + upload (this thread). */
    JceTextureCpu *c = jce_texture_decode_cpu(pak, asset_path, sampler_mode);
    JceTexture result = jce_texture_upload_cpu(c);   /* frees c */
    if (jce_texture_valid(result))
        LOG_DEBUG(LOG_TAG, "loaded %s", asset_path);
    JCE_PROFILE_ZONE_END;
    return result;
}

/* ================================================================== */
/* 3D-LUT loader (horizontal PNG strip → N×N×N RGBA8 3D texture)       */
/* ================================================================== */

/* Reorder a horizontal strip (N tiles of NxN laid left-to-right) to a
 * z-major N×N×N volume.  Tile z holds the slice where blue index == z.
 * strip_w = N*N texels per row.  Volume layout: out[(z*N + y)*N + x].
 * Declared in jce_texture_internal.h (external linkage, not JCE_API) so
 * tests/renderer/test_jce_postfx_lut.c exercises THIS code instead of a
 * copy that can pass while the shipped reorder is broken. */
void jce_lut_strip_to_volume(const uint8_t *strip, int N, uint8_t *out)
{
    int strip_w = N * N;
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int sx = z * N + x;  /* tile z, column x */
                int sy = y;
                const uint8_t *src = strip + ((size_t)sy * strip_w + sx) * 4;
                uint8_t *dst = out + (((size_t)z * N + y) * N + x) * 4;
                dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3];
            }
}

/* File-scope accessor: returns the raw RGBA8 pixels and writes dims.
 * Only valid when !c->is_cooked; returns NULL otherwise.  The buffer is
 * tightly packed (row stride == w*4), which is what the strip reorder
 * below assumes. */
static const uint8_t *texture_cpu_rgba8(const JceTextureCpu *c,
                                        int *out_w, int *out_h)
{
    if (!c || c->is_cooked || !c->rgba8) return NULL;
    if (out_w) *out_w = (int)c->rgba8_w;
    if (out_h) *out_h = (int)c->rgba8_h;
    return c->rgba8;
}

JceTexture jce_texture_load_lut_3d(const JcePakArchive *pak,
                                   const char *asset_path)
{
    JceTexture invalid = JCE_TEXTURE_INVALID;
    if (!(jce_renderer_get_caps() & JCE_CAP_TEXTURE_3D)) {
        LOG_DEBUG(LOG_TAG, "LUT: TEXTURE_3D unsupported; skipping %s", asset_path);
        return invalid;
    }
    /* Decode the strip to a CPU surface (reuses the worker decode path). */
    JceTextureCpu *c = jce_texture_decode_cpu(pak, asset_path, JCE_TEX_CLAMP);
    if (!c) return invalid;
    /* Pull RGBA8 + dims out of the decoded result.  V1 LUTs are plain PNGs,
     * so handle the raw case; fall through to abort on cooked (which v1
     * LUTs never are). */
    int img_w = 0, img_h = 0;
    const uint8_t *pixels = texture_cpu_rgba8(c, &img_w, &img_h);
    if (!pixels || img_h <= 0 || img_w != img_h * img_h) {
        LOG_WARN(LOG_TAG, "LUT strip %s wrong shape (%dx%d; need N*N x N)",
                 asset_path, img_w, img_h);
        jce_texture_cpu_free(c);
        return invalid;
    }
    int N = img_h;
    uint8_t *vol = (uint8_t *)JCE_MALLOC((size_t)N * N * N * 4);
    if (!vol) { jce_texture_cpu_free(c); return invalid; }
    jce_lut_strip_to_volume(pixels, N, vol);
    const bgfx_memory_t *mem = bgfx_copy(vol, (uint32_t)((size_t)N * N * N * 4));
    bgfx_texture_handle_t h = bgfx_create_texture_3d(
        (uint16_t)N, (uint16_t)N, (uint16_t)N, false,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP, mem, 0);
    JCE_FREE(vol);
    jce_texture_cpu_free(c);
    if (h.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "bgfx_create_texture_3d failed for LUT: %s", asset_path);
        return invalid;
    }
    LOG_DEBUG(LOG_TAG, "loaded 3D LUT %s (N=%d)", asset_path, N);
    /* Register N×N so jce_texture_get_size(lut, &w, &h) returns h==N,
     * letting the stomp derive ln=N and pass the correct lut_size to
     * jce_postfx_set_lut.  Without this the registry misses the 3D handle
     * and returns h=0, collapsing the shader UV to a uniform tint. */
    registry_add(h.idx, (uint32_t)N, (uint32_t)N, 1u, 0xFFu /* 3D LUT: not batchable */);
    JceTexture t = JCE_TEXTURE_INVALID;
    t.idx = h.idx;
    return t;
}

/* Load a 3D LUT from a loose host-filesystem file (editor loose-asset path).
 * Identical strip-decode + volume-reorder logic as jce_texture_load_lut_3d,
 * but reads via jce_fs_host_read_all instead of a PAK archive, so it works
 * in the editor where the project pak is NOT the one passed to sr_create. */
JceTexture jce_texture_load_lut_3d_host(const char *host_path)
{
    JceTexture invalid = JCE_TEXTURE_INVALID;
    if (!host_path || !host_path[0]) return invalid;
    if (!(jce_renderer_get_caps() & JCE_CAP_TEXTURE_3D)) {
        LOG_DEBUG(LOG_TAG, "LUT host: TEXTURE_3D unsupported; skipping %s", host_path);
        return invalid;
    }
    /* Read the raw file bytes from the host filesystem. */
    uint64_t file_size = 0;
    void *file_buf = jce_fs_host_read_all(host_path, &file_size);
    if (!file_buf || file_size == 0) {
        LOG_WARN(LOG_TAG, "LUT host: cannot read '%s'", host_path);
        if (file_buf) jce_fs_buffer_free(file_buf);
        return invalid;
    }
    /* Decode the in-memory PNG to RGBA8 CPU pixels. */
    JceTextureCpu *c = jce_texture_decode_cpu_mem(file_buf, (size_t)file_size,
                                                  JCE_TEX_CLAMP);
    jce_fs_buffer_free(file_buf);
    if (!c) {
        LOG_WARN(LOG_TAG, "LUT host: decode failed for '%s'", host_path);
        return invalid;
    }
    int img_w = 0, img_h = 0;
    const uint8_t *pixels = texture_cpu_rgba8(c, &img_w, &img_h);
    if (!pixels || img_h <= 0 || img_w != img_h * img_h) {
        LOG_WARN(LOG_TAG, "LUT host strip %s wrong shape (%dx%d; need N*N x N)",
                 host_path, img_w, img_h);
        jce_texture_cpu_free(c);
        return invalid;
    }
    int N = img_h;
    uint8_t *vol = (uint8_t *)JCE_MALLOC((size_t)N * N * N * 4);
    if (!vol) { jce_texture_cpu_free(c); return invalid; }
    jce_lut_strip_to_volume(pixels, N, vol);
    const bgfx_memory_t *mem = bgfx_copy(vol, (uint32_t)((size_t)N * N * N * 4));
    bgfx_texture_handle_t h = bgfx_create_texture_3d(
        (uint16_t)N, (uint16_t)N, (uint16_t)N, false,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP, mem, 0);
    JCE_FREE(vol);
    jce_texture_cpu_free(c);
    if (h.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "bgfx_create_texture_3d failed for LUT host: %s", host_path);
        return invalid;
    }
    LOG_DEBUG(LOG_TAG, "loaded 3D LUT (host) %s (N=%d)", host_path, N);
    registry_add(h.idx, (uint32_t)N, (uint32_t)N, 1u, 0xFFu /* 3D LUT: not batchable */);
    JceTexture t = JCE_TEXTURE_INVALID;
    t.idx = h.idx;
    return t;
}

/* Surface upload: mip chain + the caller's wrap mode.
 *
 * This exists because jce_texture_from_rgba() below is CLAMP and mip-0 only,
 * which is right for what it was written for -- glyph atlases, IES lookup
 * tables, video frames, ImGui thumbnails: sampled at ~1:1, and where wrapping
 * would fetch across an unrelated neighbour.  It is wrong for a MATERIAL.  A
 * material texture is tiled (terrain multiplies its UV by the layer tile
 * scale) and minified, so clamp smears the edge texel across everything past
 * the first tile, and the absent mip chain aliases whatever survives.
 *
 * Split rather than fixed in place: changing jce_texture_from_rgba() would put
 * a mip chain under every glyph atlas in the engine, and bilinear filtering
 * between glyph mips bleeds neighbouring characters into each other. */
JceTexture jce_texture_from_rgba_ex(const void *data,
                                    uint32_t width, uint32_t height,
                                    int sampler_mode)
{
    if (!data || width == 0 || height == 0)
        return JCE_TEXTURE_INVALID;

    const bgfx_memory_t *mem = alloc_mip_chain_rgba8(width, height);
    if (!mem) return JCE_TEXTURE_INVALID;
    memcpy(mem->data, data, (size_t)width * height * 4u);
    fill_mip_chain_rgba8(mem, width, height);

    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height,
        true,   /* mip chain (box-filtered above) */
        1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | sampler_flags(sampler_mode),
        mem, 0);

    if (handle.idx == UINT16_MAX)
        return JCE_TEXTURE_INVALID;

    registry_add(handle.idx, width, height, full_mip_count(width, height),
                 (uint8_t)BGFX_TEXTURE_FORMAT_RGBA8);

    JceTexture tex;
    tex.idx = handle.idx;
    return tex;
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
        NULL, 0);

    if (handle.idx == UINT16_MAX)
        return JCE_TEXTURE_INVALID;

    registry_add(handle.idx, width, height, 1u,
                 (uint8_t)BGFX_TEXTURE_FORMAT_RGBA8);

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

JceTexture jce_texture_from_cooked(const JceAssetTexInfo *info,
                                   const void *pixels, size_t pixel_bytes,
                                   int sampler_mode)
{
    if (!info || !pixels || pixel_bytes == 0 ||
        info->width == 0 || info->height == 0)
        return JCE_TEXTURE_INVALID;

    /* Reject a pixel chunk too small for the declared dimensions/format/mips:
     * bgfx_create_texture_2d reads the implied size from `mem`, so a short
     * chunk causes an OOB read during upload (audit R2-texture-trust-wh). */
    uint32_t need = jce_tex_cooked_pixel_size(info->width, info->height,
                                              (int)info->format,
                                              info->mip_count);
    if (need == 0 || pixel_bytes != need)
        return JCE_TEXTURE_INVALID;

    /* Block-compressed and multi-mip payloads are uploaded as a single
     * memory blob: when has_mips is true, bgfx consumes the concatenated
     * mip chain from `mem` itself (same convention as the synchronous
     * cooked loader). This is what the async finalize path was missing —
     * previously it forced RGBA8 and dropped mips, corrupting every
     * BC/ASTC texture emitted by Build Bundles. */
    bool has_mips = info->mip_count > 1;
    bgfx_texture_format_t bgfx_fmt = texfmt_to_bgfx(info->format);
    if (bgfx_fmt == BGFX_TEXTURE_FORMAT_COUNT)
        return JCE_TEXTURE_INVALID;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !(caps->formats[bgfx_fmt] & BGFX_CAPS_FORMAT_TEXTURE_2D))
        return JCE_TEXTURE_INVALID;

    const bgfx_memory_t *mem = bgfx_alloc((uint32_t)pixel_bytes);
    memcpy(mem->data, pixels, pixel_bytes);

    /* A caller that asked for sRGB gets it only if this format has an sRGB
     * view; otherwise the flag is dropped HERE rather than passed to bgfx to
     * be ignored, and the log says which texture and which format, because
     * the visible symptom (one over-bright surface) points at the art. */
    if ((sampler_mode & JCE_TEX_SRGB) && !format_has_srgb(bgfx_fmt)) {
        LOG_WARN(LOG_TAG, "sRGB requested for a format with no sRGB view "
                          "(bgfx format %d); sampling it linear",
                 (int)bgfx_fmt);
        sampler_mode &= ~JCE_TEX_SRGB;
    }
    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)info->width, (uint16_t)info->height,
        has_mips, 1, bgfx_fmt,
        BGFX_TEXTURE_NONE | sampler_flags(sampler_mode), mem, 0);

    if (handle.idx == UINT16_MAX)
        return JCE_TEXTURE_INVALID;

    registry_add(handle.idx, info->width, info->height,
                 (uint8_t)(info->mip_count > 0u ? info->mip_count : 1u),
                 (uint8_t)bgfx_fmt);   /* cooked: RGBA8 or block-compressed */
    /* VRAM ceiling: only the uncompressed RGBA8 cooked format keeps a CPU
     * source for streaming demote — its mip-0 lives at offset 0 of the chunk as
     * plain RGBA8 (the box-filter downsample is RGBA8-only).  Block-compressed
     * (BC/ASTC/ETC2) and RGB8 are NOT opted in (no RGBA8 source to downsample;
     * the demote would be a truthful no-op anyway). */
    if (info->format == JCEASSET_TEXFMT_RGBA8)
        registry_opt_in_streaming(handle.idx, info->width, info->height,
                                  pixels, sampler_mode);

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

/* Upload caller-owned RGBA8 into an existing texture.
 *
 * HISTORY: this used bgfx_make_ref (zero-copy) with NO release callback,
 * which imposed a "data must stay valid until bgfx_frame()" contract that
 * the sole callers — the runtime video component (jce_scene_video.c) and
 * the editor video viewer — could not honour: jce_video_unload / the
 * display_rgba realloc free the buffer immediately (same frame) from flecs
 * dtor/move hooks, entity delete, scene teardown and clip-path edits, so a
 * video entity deleted on the frame it uploads left bgfx reading freed
 * memory at submit (a reproducible UAF, worsened by mimalloc page purge).
 * bgfx_copy makes bgfx own the pixels, removing the lifetime coupling.  The
 * cost is one memcpy per *decoded* video frame (video fps, not render fps);
 * a future zero-copy path would need bgfx_make_ref_release + a refcounted
 * buffer or a 2-frame deferred-free list (see audit R-D41 / video-frame-ref-uaf). */
bool jce_texture_update_rgba_ref(JceTexture tex, const void *data,
                                 uint32_t width, uint32_t height)
{
    if (!jce_texture_valid(tex) || !data || width == 0 || height == 0)
        return false;

    TexEntry *e = registry_find(tex.idx);
    if (e && (e->width != width || e->height != height))
        return false;

    const uint32_t bytes = width * height * 4u;
    const bgfx_memory_t *mem = bgfx_copy(data, bytes);

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

uint32_t jce_texture_get_mips(JceTexture tex)
{
    TexEntry *e = registry_find(tex.idx);
    return (e && e->mip_count > 0) ? e->mip_count : 1u;
}

uint32_t jce_texture_get_format(JceTexture tex)
{
    TexEntry *e = registry_find(tex.idx);
    return e ? (uint32_t)e->bgfx_fmt : UINT32_MAX;
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
    if ((int)s_global_mip_bias      > b) b = s_global_mip_bias;
    if ((int)s_texture_quality_bias > b) b = s_texture_quality_bias;
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
    /* Clamp the +1 taps to the source extents: for odd/1-px source levels
     * the unclamped p10/p01/p11 read past the buffer (an intermittent-AV /
     * garbage-average source on the mip tail). Even dimensions >= 2 are
     * byte-identical to the unclamped version. */
    for (uint32_t y = 0; y < dh; y++) {
        const uint32_t y0 = y * 2u;
        const uint32_t y1 = (y0 + 1u < sh) ? y0 + 1u : y0;
        for (uint32_t x = 0; x < dw; x++) {
            const uint32_t x0 = x * 2u;
            const uint32_t x1 = (x0 + 1u < sw) ? x0 + 1u : x0;
            const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * 4u;
            const uint8_t *p10 = src + ((size_t)y0 * sw + x1) * 4u;
            const uint8_t *p01 = src + ((size_t)y1 * sw + x0) * 4u;
            const uint8_t *p11 = src + ((size_t)y1 * sw + x1) * 4u;
            uint8_t *o = dst + ((size_t)y * dw + x) * 4u;
            for (int c = 0; c < 4; c++)
                o[c] = (uint8_t)(((unsigned)p00[c] + p10[c] + p01[c] + p11[c]) >> 2);
        }
    }
    *out_dw = dw;
    *out_dh = dh;
    return dst;
}

/* CPU half (worker-safe — NO bgfx): box-filter the cached mip-0 down to
 * `target`.  On success *out_px is the buffer to upload (+ dims); *out_owned
 * is true when it must be freed after upload (false only at target 0, where
 * it aliases e->source_pixels).  Returns false if no source is cached. */
static bool mip_downsample_cpu(const TexEntry *e, uint8_t target,
                               uint8_t **out_px, uint32_t *out_w,
                               uint32_t *out_h, bool *out_owned)
{
    if (!e->has_source_pixels || !e->source_pixels) return false;

    uint32_t cw = e->base_width, ch = e->base_height;
    uint8_t *cur = e->source_pixels;
    bool owned = false;
    for (uint8_t i = 0; i < target; i++) {
        uint32_t nw, nh;
        uint8_t *next = downsample_rgba8(cur, cw, ch, &nw, &nh);
        if (owned) JCE_FREE(cur);
        if (!next) return false;
        cur = next; owned = true; cw = nw; ch = nh;
    }
    *out_px = cur; *out_w = cw; *out_h = ch; *out_owned = owned;
    return true;
}

/* GPU half (render thread): recreate the texture from downsampled pixels,
 * freeing `px` when `owned`.  Updates the registry entry.  Returns true if
 * the bgfx handle was recreated. */
static bool mip_upload_gpu(TexEntry *e, uint8_t *px, uint32_t w, uint32_t h,
                           uint8_t target, bool owned)
{
    bgfx_texture_handle_t old_h; old_h.idx = e->idx;
    bgfx_destroy_texture(old_h);

    const bgfx_memory_t *mem = bgfx_alloc(w * h * 4u);
    memcpy(mem->data, px, (size_t)w * h * 4u);
    if (owned) JCE_FREE(px);

    bgfx_texture_handle_t nh = bgfx_create_texture_2d(
        (uint16_t)w, (uint16_t)h,
        false, 1, BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | sampler_flags(e->sampler_mode),
        mem, 0);

    if (nh.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG,
            "mip drop reupload failed for tex idx=%u (-> %ux%u)",
            e->idx, w, h);
        return false;
    }

    /* bgfx may hand back a different idx — reflect that in registry. */
    e->idx              = nh.idx;
    e->width            = w;
    e->height           = h;
    e->resident_top_mip = target;
    return true;
}

/* Apply a target top_mip to the GPU resource using cached source pixels.
 * Synchronous (downsample + upload on the calling thread).  Used by the
 * per-texture streaming entry points. */
static bool apply_top_mip(TexEntry *e, uint8_t target_top_mip)
{
    if (target_top_mip == e->resident_top_mip) return false;

    uint8_t *px = NULL; uint32_t w = 0, h = 0; bool owned = false;
    if (!mip_downsample_cpu(e, target_top_mip, &px, &w, &h, &owned)) {
        /* No CPU mip-0 source retained → we cannot actually shrink the GPU
         * texture, so we must NOT pretend we did.  The request stays recorded
         * as intent (desired_top_mip / per_texture_bias are already set by the
         * caller); resident_top_mip, width and height keep reflecting the real
         * GPU state.  Honest demotion engages for source-backed textures only
         * (audit F29: never report a VRAM shrink that did not happen). */
        return false;
    }
    return mip_upload_gpu(e, px, w, h, target_top_mip, owned);
}

/* ── Parallel global-bias recompute ───────────────────────────────── */

typedef struct {
    TexEntry *e;
    uint8_t   target;
    uint8_t  *pixels;   /* downsampled (worker) */
    uint32_t  w, h;
    bool      owned;
    bool      ok;
} MipRecompute;

/* Worker: downsample entries [begin,end) (pure CPU, disjoint writes). */
static void mip_downsample_range(uint32_t begin, uint32_t end, void *user)
{
    MipRecompute *list = (MipRecompute *)user;
    for (uint32_t i = begin; i < end; i++) {
        MipRecompute *m = &list[i];
        m->ok = mip_downsample_cpu(m->e, m->target,
                                   &m->pixels, &m->w, &m->h, &m->owned);
    }
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

void jce_texture_set_quality_mip_bias(int8_t bias)
{
    s_texture_quality_bias = clamp_bias(bias);
}

void jce_texture_set_colour_space(int linear)
{
    s_colour_space_linear = linear ? 1 : 0;
}

int jce_texture_colour_space(void)
{
    return s_colour_space_linear;
}

void jce_texture_set_aniso_override(int mode)
{
    /* Store as-is: <0 tier default, 0 off, >0 forced on. */
    s_aniso_override = (int8_t)(mode < -1 ? -1 : (mode > 2 ? 2 : mode));
}

void jce_texture_set_global_mip_bias(int8_t bias)
{
    s_global_mip_bias     = clamp_bias(bias);
    s_global_mip_bias_set = true;

    /* A global bias change can recompute many textures at once, and the
     * box-filter downsample is the bulk of that work.  Build the change
     * list, downsample the CPU pixels in parallel (no bgfx), then recreate
     * the GPU textures serially on this (render) thread.  Untracked
     * textures stay at full residency (preserves opt-in behaviour). */
    MipRecompute *list =
        (MipRecompute *)JCE_MALLOC((size_t)(s_count > 0 ? s_count : 1) *
                                   sizeof(MipRecompute));
    if (!list) {
        /* OOM: fall back to the per-entry synchronous path. */
        for (int i = 0; i < s_count; i++) {
            TexEntry *e = &s_registry[i];
            if (e->streaming_tracked) apply_top_mip(e, effective_top_mip(e));
        }
        return;
    }

    int n = 0;
    for (int i = 0; i < s_count; i++) {
        TexEntry *e = &s_registry[i];
        if (!e->streaming_tracked) continue;
        uint8_t target = effective_top_mip(e);
        if (target == e->resident_top_mip) continue;
        if (!e->has_source_pixels || !e->source_pixels) {
            /* No retained source → cannot demote; leave residency truthful
             * (audit F29: never report a GPU shrink that did not happen). */
            continue;
        }
        list[n].e = e; list[n].target = target;
        list[n].pixels = NULL; list[n].ok = false;
        n++;
    }
    if (n == 0) { JCE_FREE(list); return; }

    /* Pass A: parallel CPU downsample (worker-safe, disjoint per-index). */
    JceThreadPool *pool = jce_thread_pool_shared();
    if (pool && n >= 4)
        jce_thread_pool_parallel_for_named(
            pool, "texture.mip-downsample", (uint32_t)n, 0,
            mip_downsample_range, list);
    else
        mip_downsample_range(0u, (uint32_t)n, list);

    /* Pass B: serial GPU recreate on this (render) thread. */
    for (int i = 0; i < n; i++) {
        MipRecompute *m = &list[i];
        if (m->ok)
            mip_upload_gpu(m->e, m->pixels, m->w, m->h, m->target, m->owned);
        /* else: downsample failed → GPU untouched; keep residency truthful
         * (audit F29). */
    }
    JCE_FREE(list);
}

int8_t jce_texture_get_global_mip_bias(void)
{
    return s_global_mip_bias;
}
