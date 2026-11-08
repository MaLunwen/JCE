/*
 * jce_texture.c  Cross-platform texture loading implementation.
 *
 * Pipeline: PAK → decompress → jce_image decode → RGBA8 → bgfx texture.
 * The decode itself belongs to the jce_image service — this file never
 * picks a codec.
 */

#include <jce/os/core/jce_log.h>
#include "jce_upload_metrics.h"
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_image.h>    /* the one image-decode service */
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_asset_format.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_filesystem.h> /* jce_fs_host_read_all (LUT host loader) */

#include "jce_texture_mips.h"
#include "jce_texture_internal.h"      /* jce_lut_strip_to_volume decl */
#include "jce_texture_diagnostics_internal.h"
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
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
    bool     replacement_warned;  /* raw by-value handles cannot be rebound */
    uint32_t replacement_trace_generation;
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

/* Existing streaming upload opt-in retains a CPU RGBA8 mip-0 mirror and
 * records intent. Dynamic raw-handle replacement is unsupported; retaining
 * a source does not currently reclaim residency memory. Off by default so
 * editor/UI/one-off textures pay no extra mirror RAM. Owner-thread only. */
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

/* Preserve the existing opt-in CPU source ownership and streaming intent.
 * This mirror does not enable safe runtime raw-handle replacement. Called
 * only when s_streaming_uploads is armed (runtime streamed uploads).
 * `rgba8` is the tightly-packed (w*h*4) mip-0; a private copy is taken. */
static void registry_opt_in_streaming(uint16_t idx, uint32_t w, uint32_t h,
                                      const void *rgba8, int sampler_mode)
{
    if (!s_streaming_uploads || !rgba8 || w == 0 || h == 0) return;
    TexEntry *e = registry_find(idx);
    if (!e || e->has_source_pixels) return;
    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *copy = (uint8_t *)JCE_MALLOC(bytes);
    if (!copy) return;   /* best-effort retained source; no residency claim */
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
    size_t total;
    if (!jce_texture_mip_bytes(w, h, &total)) return NULL;
    return bgfx_alloc((uint32_t)total);
}

/* Levels 1..N-1, box-filtered in place.  Mip 0 must already be written. */
static void fill_mip_chain_rgba8(const bgfx_memory_t *mem, uint32_t w, uint32_t h)
{
    if (mem) jce_texture_mip_fill(mem->data, mem->size, w, h);
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
     * downsample_rgba8 supports initial tier-size selection. Runtime residency
     * replacement cannot reclaim this published resource safely. */
    const bgfx_memory_t *mem = alloc_mip_chain_rgba8(w, h);
    if (!mem) { JCE_FREE(shrunk); return JCE_TEXTURE_INVALID; }
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

    const uint64_t payload_bytes_6 = mem ? mem->size : 0u;
    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)w, (uint16_t)h,
        true,   /* mip chain (box-filtered above) → trilinear minification */
        1,      /* layers */
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | sampler_flags(mode),
        mem, 0);
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_CREATE, payload_bytes_6,
        handle.idx != UINT16_MAX);

    if (handle.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "bgfx_create_texture_2d failed (%ux%u)", w, h);
        return JCE_TEXTURE_INVALID;
    }

    registry_add(handle.idx, w, h, full_mip_count(w, h),
                 (uint8_t)BGFX_TEXTURE_FORMAT_RGBA8);
    /* Existing streamed RGBA8 opt-in retains a CPU source and intent. It does
     * not enable safe replacement of the published raw handle. mem->data
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
 * packed original-resolution RGBA8 mip chain prepared without GPU calls.
 * upload_cpu/cpu_free release the JCE-owned payload; rgba8 aliases mip 0. */
struct JceTextureCpu {
    int             sampler_mode;
    bool            is_cooked;
    JceAssetTexInfo info;          /* cooked */
    void           *pixels;        /* owned cooked payload or raw full mip chain */
    size_t          pixel_bytes;
    uint8_t        *rgba8;         /* raw: mip-0 alias into owned pixels chain */
    uint32_t        rgba8_w;
    uint32_t        rgba8_h;
    JceTextureRequestIdentity identity;
};

static JceTexture texture_from_cooked_request(const JceAssetTexInfo *info,
    const void *pixels,size_t pixel_bytes,int sampler_mode,JceTextureRequestIdentity identity);

static JceTextureCpu *texture_decode_owned(void *buf, size_t n,
                                           int sampler_mode, const char *asset_path);

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

    return texture_decode_owned(buf, n, sampler_mode, asset_path);
}

/* Both PAK and borrowed-memory/VFS paths accept cooked bytes. This owns buf
 * and releases it on every path, using the actual decompressed byte count. */
static JceTextureCpu *texture_decode_owned(void *buf, size_t n,
                                           int sampler_mode, const char *asset_path)
{
    JceTextureRequestIdentity identity=texture_identity(asset_path);
    /* ── Cooked path: .jceasset ── */
    if (jce_asset_is_cooked(buf, n)) {
        JceAssetView view;
        if (!jce_asset_open(&view, buf, n)) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_HEADER,NULL,n,0,
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(buf);
            return NULL;
        }

        const JceAssetChunkEntry *info_chunk =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
        const JceAssetChunkEntry *pixel_chunk =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);

        identity.source_hash=view.header->source_hash;
        if (view.header->asset_type!=JCEASSET_TYPE_TEXTURE || !info_chunk || !pixel_chunk) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_CHUNKS,NULL,n,0,
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(buf);
            return NULL;
        }

        /* Reject short metadata before memcpy, including on 32-bit Web. */
        if(info_chunk->original_size<sizeof(JceAssetTexInfo) ||
           info_chunk->original_size>SIZE_MAX) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_INFO,NULL,n,sizeof(JceAssetTexInfo),
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(buf);return NULL;
        }
        if(pixel_chunk->original_size==0 || pixel_chunk->original_size>SIZE_MAX) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_PIXELS,NULL,n,0,
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(buf);return NULL;
        }
        /* Read texture info (info chunk may include mip offsets after the struct). */
        void *info_buf = JCE_MALLOC((size_t)info_chunk->original_size);
        if (!info_buf) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_DECODE_ALLOC,NULL,n,0,
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(buf); return NULL;
        }

        if (jce_asset_chunk_data(&view, info_chunk,
                                  info_buf, (size_t)info_chunk->original_size) < sizeof(JceAssetTexInfo)) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_INFO,NULL,n,sizeof(JceAssetTexInfo),
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
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
        if (!tex_data) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_DECODE_ALLOC,&tex_info,pixel_bytes,0,
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(buf); return NULL;
        }

        if (jce_asset_chunk_data(&view, pixel_chunk,
                                  tex_data, pixel_bytes) != pixel_bytes) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_PIXELS,&tex_info,pixel_bytes,0,
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(tex_data);
            JCE_FREE(buf);
            return NULL;
        }
        JCE_FREE(buf); /* PAK buffer no longer needed */

        JceTextureCpu *c = JCE_MALLOC(sizeof(*c));
        if (!c) {
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_DECODE_ALLOC,&tex_info,pixel_bytes,0,
                BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
            JCE_FREE(tex_data); return NULL;
        }
        memset(c, 0, sizeof(*c));
        c->sampler_mode = sampler_mode;
        c->is_cooked    = true;
        c->info         = tex_info;
        c->pixels       = tex_data;
        c->pixel_bytes  = pixel_bytes;
        c->identity     = identity;
        return c;
    }

    /* ── Raw path: PNG/JPG → jce_image → RGBA8 ── */
    int img_w = 0, img_h = 0;
    uint8_t *rgba8 = jce_image_load_rgba8_from_memory(buf, (uint64_t)n,
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
    c->identity = identity;
    c->is_cooked    = false;
    size_t chain_bytes;
    if (img_w <= 0 || img_h <= 0 || img_w > UINT16_MAX || img_h > UINT16_MAX ||
        !jce_texture_mip_bytes((uint32_t)img_w, (uint32_t)img_h, &chain_bytes)) {
        jce_image_free_rgba8(rgba8); JCE_FREE(c); return NULL;
    }
    c->pixels = JCE_MALLOC(chain_bytes);
    if (!c->pixels) { jce_image_free_rgba8(rgba8); JCE_FREE(c); return NULL; }
    c->pixel_bytes = chain_bytes;
    memcpy(c->pixels, rgba8, (size_t)img_w * (size_t)img_h * 4u);
    jce_image_free_rgba8(rgba8);
    if (!jce_texture_mip_fill(c->pixels, chain_bytes, (uint32_t)img_w, (uint32_t)img_h)) {
        JCE_FREE(c->pixels); JCE_FREE(c); return NULL;
    }
    c->rgba8 = c->pixels;
    c->rgba8_w = (uint32_t)img_w;
    c->rgba8_h = (uint32_t)img_h;
    return c;
}

JceTextureCpu *jce_texture_decode_cpu_mem(const void *encoded, size_t size,
                                          int sampler_mode)
{
    if (!encoded || size == 0) return NULL;
    void *owned = JCE_MALLOC(size);
    if (!owned) return NULL;
    memcpy(owned, encoded, size);
    return texture_decode_owned(owned, size, sampler_mode, "(memory)");
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
        tex = texture_from_cooked_request(&c->info,c->pixels,c->pixel_bytes,
                                          c->sampler_mode,c->identity);
    } else if (c->rgba8) {
        uint32_t w = c->rgba8_w, h = c->rgba8_h;
        uint32_t cap = 0;
        if (c->sampler_mode != JCE_TEX_CLAMP) {
            JceRenderRecommendation recommendation = jce_renderer_get_recommendation();
            if (recommendation.max_texture_size >= 256u) cap = recommendation.max_texture_size;
        }
        size_t offset = jce_texture_mip_select(&w, &h, cap);
        if (offset < c->pixel_bytes) {
            const uint8_t *base = (const uint8_t *)c->pixels + offset;
            const bgfx_memory_t *mem = bgfx_copy(base, (uint32_t)(c->pixel_bytes - offset));
            if (mem) {
                const uint64_t payload_bytes_5 = mem ? mem->size : 0u;
                bgfx_texture_handle_t handle = bgfx_create_texture_2d((uint16_t)w,
                    (uint16_t)h, true, 1, BGFX_TEXTURE_FORMAT_RGBA8,
                    BGFX_TEXTURE_NONE | sampler_flags(c->sampler_mode), mem, 0);
                jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_CREATE, payload_bytes_5,
                    handle.idx != UINT16_MAX);
                if (handle.idx != UINT16_MAX) {
                    registry_add(handle.idx, w, h, full_mip_count(w, h),
                        (uint8_t)BGFX_TEXTURE_FORMAT_RGBA8);
                    registry_opt_in_streaming(handle.idx, w, h, base, c->sampler_mode);
                    tex.idx = handle.idx;
                }
            }
        }
    }
    jce_texture_cpu_free(c);
    return tex;
}

void jce_texture_cpu_free(JceTextureCpu *c)
{
    if (!c) return;
    /* Each buffer goes back to the allocator that produced it: the cooked
     * payload and prepared raw chain are ours; raw mip-0 is an alias. */
    if (c->pixels) JCE_FREE(c->pixels);
    /* Raw rgba8 aliases pixels; the image-service allocation was freed on decode. */
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
    const uint64_t payload_bytes_4 = mem ? mem->size : 0u;
    bgfx_texture_handle_t h = bgfx_create_texture_3d(
        (uint16_t)N, (uint16_t)N, (uint16_t)N, false,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP, mem, 0);
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_CREATE, payload_bytes_4,
        h.idx != UINT16_MAX);
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
    const uint64_t payload_bytes_3 = mem ? mem->size : 0u;
    bgfx_texture_handle_t h = bgfx_create_texture_3d(
        (uint16_t)N, (uint16_t)N, (uint16_t)N, false,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_W_CLAMP, mem, 0);
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_CREATE, payload_bytes_3,
        h.idx != UINT16_MAX);
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

    const uint64_t payload_bytes_2 = mem ? mem->size : 0u;
    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)width, (uint16_t)height,
        true,   /* mip chain (box-filtered above) */
        1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_NONE | sampler_flags(sampler_mode),
        mem, 0);
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_CREATE, payload_bytes_2,
        handle.idx != UINT16_MAX);

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
    const uint64_t payload_update_bytes_9 = mem ? mem->size : 0u;
    bgfx_update_texture_2d(handle,
                           0, /* layer */
                           0, /* mip */
                           0, /* x */
                           0, /* y */
                           (uint16_t)width,
                           (uint16_t)height,
                           mem,
                           (uint16_t)(width * 4u));
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_UPDATE, payload_update_bytes_9, true);

    JceTexture tex;
    tex.idx = handle.idx;
    return tex;
}

JceTexture jce_texture_from_cooked(const JceAssetTexInfo *info,
                                   const void *pixels, size_t pixel_bytes,
                                   int sampler_mode)
{
    const JceTextureRequestIdentity identity={0,0};
    return texture_from_cooked_request(info,pixels,pixel_bytes,sampler_mode,identity);
}

/* Expand advertised sampled BC3/BC5 emulation explicitly so the resulting
 * texture can use the destination format's sRGB capability and metadata. */
static bool decode_sampled_bc_mips(const JceAssetTexInfo *info,
                                  const uint8_t *blocks, uint8_t *rgba)
{
    uint8_t *stripe = NULL;
    uint32_t mips = info->mip_count > 0u ? info->mip_count : 1u;
    for (uint32_t mip = 0; mip < mips; ++mip) {
        uint32_t w, h;
        jce_tex_mip_dimensions(info->width, info->height, mip, &w, &h);
        if (((w | h) & 3u) == 0u) {
            if (!jce_tex_decode_to_rgba8(blocks, w, h, (int)info->format, rgba))
                goto failed;
        } else {
            /* The block decoder takes whole blocks, including edge mips.
             * Crop one block row at a time to keep scratch memory bounded. */
            uint32_t padded_w = (w + 3u) & ~3u;
            if (!stripe) {
                uint32_t widest = (info->width + 3u) & ~3u;
                stripe = (uint8_t *)JCE_MALLOC((size_t)widest * 16u);
                if (!stripe)
                    goto failed;
            }
            for (uint32_t y = 0; y < h; y += 4u) {
                size_t offset = (size_t)(y / 4u) * padded_w * 4u;
                if (!jce_tex_decode_to_rgba8(blocks + offset, padded_w, 4u,
                                            (int)info->format, stripe))
                    goto failed;
                uint32_t rows = h - y < 4u ? h - y : 4u;
                for (uint32_t row = 0; row < rows; ++row)
                    memcpy(rgba + (size_t)(y + row) * w * 4u,
                           stripe + (size_t)row * padded_w * 4u, (size_t)w * 4u);
            }
        }
        blocks += jce_tex_cooked_pixel_size(w, h, (int)info->format, 1u);
        rgba += (size_t)w * h * 4u;
    }
    JCE_FREE(stripe);
    return true;

failed:
    JCE_FREE(stripe);
    return false;
}

static JceTexture texture_from_cooked_request(const JceAssetTexInfo *info,
    const void *pixels,size_t pixel_bytes,int sampler_mode,JceTextureRequestIdentity identity)
{
    if (!info || !pixels || pixel_bytes == 0 ||
        pixel_bytes>UINT32_MAX || info->width == 0 || info->height == 0 ||
        info->width>UINT16_MAX || info->height>UINT16_MAX) {
        texture_request_rejected(identity,JCE_TEXTURE_FAIL_LAYOUT,info,pixel_bytes,0,
            BGFX_TEXTURE_FORMAT_COUNT,NULL,-1);
        return JCE_TEXTURE_INVALID;
    }
    bgfx_texture_format_t bgfx_fmt = texfmt_to_bgfx(info->format);
    if (bgfx_fmt == BGFX_TEXTURE_FORMAT_COUNT) {
        texture_request_rejected(identity,JCE_TEXTURE_FAIL_FORMAT,info,pixel_bytes,0,
            bgfx_fmt,NULL,-1);
        return JCE_TEXTURE_INVALID;
    }

    /* Reject a pixel chunk too small for the declared dimensions/format/mips:
     * bgfx_create_texture_2d reads the implied size from `mem`, so a short
     * chunk causes an OOB read during upload (audit R2-texture-trust-wh). */
    uint32_t need = jce_tex_cooked_pixel_size(info->width, info->height,
                                              (int)info->format,
                                              info->mip_count);
    if (need == 0 || pixel_bytes != need) {
        texture_request_rejected(identity,JCE_TEXTURE_FAIL_LAYOUT,info,pixel_bytes,need,
            bgfx_fmt,NULL,-1);
        return JCE_TEXTURE_INVALID;
    }

    /* Block-compressed and multi-mip payloads are uploaded as a single
     * memory blob: when has_mips is true, bgfx consumes the concatenated
     * mip chain from `mem` itself (same convention as the synchronous
     * cooked loader). This is what the async finalize path was missing —
     * previously it forced RGBA8 and dropped mips, corrupting every
     * BC/ASTC texture emitted by Build Bundles. */
    bool has_mips = info->mip_count > 1;
    const bgfx_caps_t *caps = bgfx_get_caps();
    const int backend=caps ? (int)bgfx_get_renderer_type() : -1;
    uint32_t format_caps = caps ? caps->formats[bgfx_fmt] : 0u;
    bool native_2d = (format_caps & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0;
    uint32_t rgba_bytes = 0;
    if (!native_2d && (format_caps & BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED) &&
        (info->format == JCEASSET_TEXFMT_BC3 || info->format == JCEASSET_TEXFMT_BC5) &&
        (caps->formats[BGFX_TEXTURE_FORMAT_RGBA8] & BGFX_CAPS_FORMAT_TEXTURE_2D) &&
        info->width <= caps->limits.maxTextureSize &&
        info->height <= caps->limits.maxTextureSize &&
        (!has_mips || info->mip_count == jce_tex_mip_count(info->width, info->height))) {
        /* Older devices can advertise a maximum size much larger than the
         * memory this one-time CPU expansion should reserve. */
        const uint32_t max_emulated_bytes = 64u * 1024u * 1024u;
        rgba_bytes = jce_tex_cooked_pixel_size(info->width, info->height,
                                               JCEASSET_TEXFMT_RGBA8, info->mip_count);
        if (rgba_bytes > max_emulated_bytes)
            rgba_bytes = 0;
    }
    if (!native_2d && rgba_bytes == 0) {
        texture_request_rejected(identity,JCE_TEXTURE_FAIL_CAPS,info,pixel_bytes,need,
            bgfx_fmt,caps,backend);
        return JCE_TEXTURE_INVALID;
    }

    uint8_t *decoded = NULL;
    uint32_t upload_bytes = (uint32_t)pixel_bytes;
    if (rgba_bytes != 0) {
        decoded = (uint8_t *)JCE_MALLOC(rgba_bytes);
        if (!decoded || !decode_sampled_bc_mips(info, (const uint8_t *)pixels, decoded)) {
            JCE_FREE(decoded);
            texture_request_rejected(identity,JCE_TEXTURE_FAIL_UPLOAD_ALLOC,info,pixel_bytes,need,
                bgfx_fmt,caps,backend);
            return JCE_TEXTURE_INVALID;
        }
        bgfx_fmt = BGFX_TEXTURE_FORMAT_RGBA8;
        upload_bytes = rgba_bytes;
    }
    const bgfx_memory_t *mem = bgfx_alloc(upload_bytes);
    if (!mem) {
        JCE_FREE(decoded);
        texture_request_rejected(identity,JCE_TEXTURE_FAIL_UPLOAD_ALLOC,info,pixel_bytes,need,
            bgfx_fmt,caps,backend);
        return JCE_TEXTURE_INVALID;
    }
    memcpy(mem->data, decoded ? decoded : pixels, upload_bytes);
    JCE_FREE(decoded);

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
    const uint64_t payload_bytes_1 = mem ? mem->size : 0u;
    bgfx_texture_handle_t handle = bgfx_create_texture_2d(
        (uint16_t)info->width, (uint16_t)info->height,
        has_mips, 1, bgfx_fmt,
        BGFX_TEXTURE_NONE | sampler_flags(sampler_mode), mem, 0);
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_CREATE, payload_bytes_1,
        handle.idx != UINT16_MAX);

    if (handle.idx == UINT16_MAX) {
        texture_request_rejected(identity,JCE_TEXTURE_FAIL_UPLOAD_CREATE,info,pixel_bytes,need,
            bgfx_fmt,caps,backend);
        return JCE_TEXTURE_INVALID;
    }

    registry_add(handle.idx, info->width, info->height,
                 (uint8_t)(info->mip_count > 0u ? info->mip_count : 1u),
                 (uint8_t)bgfx_fmt);   /* cooked: RGBA8 or block-compressed */
    /* Only the uncompressed RGBA8 cooked format retains the existing CPU
     * source mirror — its mip-0 lives at offset 0 of the chunk as
     * plain RGBA8 (the box-filter downsample is RGBA8-only).  Block-compressed
     * (BC/ASTC/ETC2) and RGB8 are NOT opted in (no RGBA8 source to downsample;
     * dynamic replacement is unsupported for all raw borrowed handles). */
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
    const uint64_t payload_update_bytes_8 = mem ? mem->size : 0u;
    bgfx_update_texture_2d(handle,
                           0, /* layer */
                           0, /* mip */
                           0, /* x */
                           0, /* y */
                           (uint16_t)width,
                           (uint16_t)height,
                           mem,
                           (uint16_t)(width * 4u));
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_UPDATE, payload_update_bytes_8, true);
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
    const uint64_t payload_update_bytes_7 = mem ? mem->size : 0u;
    bgfx_update_texture_2d(handle,
                           0, /* layer */
                           0, /* mip */
                           0, /* x */
                           0, /* y */
                           (uint16_t)width,
                           (uint16_t)height,
                           mem,
                           (uint16_t)(width * 4u));
    jce_upload_metrics_record(JCE_UPLOAD_TEXTURE_UPDATE, payload_update_bytes_7, true);
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
/* Runtime requests retain intent. JceTexture is a borrowed raw bgfx handle
 * copied into materials, scene components and UI consumers; the registry has
 * no indirection that can rebind all those copies. Never destroy/recreate a
 * published handle to change residency. Initial upload still chooses its
 * quality/size/mip chain before publishing the handle. Dynamic replacement
 * needs a separate stable-handle contract; do not report reclaimed memory. */

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
    jce_texture_mip_downsample(src, sw, sh, dst);
    *out_dw = dw;
    *out_dh = dh;
    return dst;
}

/* No portable transaction can replace a raw handle borrowed by value.
 * Keep the owned source, dimensions, registry mapping and resident mip intact.
 * A single diagnostic per entry and one cancelled trace per capture expose
 * the unsupported outcome without per-frame log/trace flooding. */
static bool apply_top_mip(TexEntry *e, uint8_t target_top_mip)
{
    if (!e || target_top_mip == e->resident_top_mip) return false;
    if (!e->replacement_warned) {
        LOG_WARN(LOG_TAG,
            "residency phase=unsupported reason=borrowed_raw_handle "
            "handle=%u requested_top_mip=%u resident_top_mip=%u",
            (unsigned)e->idx, (unsigned)target_top_mip,
            (unsigned)e->resident_top_mip);
        e->replacement_warned = true;
    }
    if (jce_trace_enabled()) {
        const uint32_t generation = jce_trace_capture_generation();
        if (e->replacement_trace_generation != generation) {
            JceTraceScope scope = jce_trace_scope_begin("texture.residency.unsupported");
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "texture.residency.requested_top_mip", target_top_mip, scope.id, scope.parent);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "texture.residency.resident_top_mip", e->resident_top_mip, scope.id, scope.parent);
            jce_trace_scope_end(scope, JCE_TRACE_TASK_CANCELLED);
            e->replacement_trace_generation = generation;
        }
    }
    return false;
}

/* Keep late opt-in intent without claiming a source mirror or replacement. */
static void ensure_source_cached(TexEntry *e)
{
    if (e->has_source_pixels) return;
    /* No portable GPU readback path in this engine — when a texture
     * opts in late, we record the intent but cannot reconstitute mip 0
     * from the GPU. Source-backed entries retain their existing bytes, but
     * both paths remain unchanged until a separately owned reload publishes
     * a new handle to its consumers. */
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

    /* Preserve every published raw handle. No CPU downsample, staging blob,
     * replacement allocation or old-handle retirement is permitted here. */
    for (int i = 0; i < s_count; ++i) {
        TexEntry *e = &s_registry[i];
        if (e->streaming_tracked) apply_top_mip(e, effective_top_mip(e));
    }
}

int8_t jce_texture_get_global_mip_bias(void)
{
    return s_global_mip_bias;
}
