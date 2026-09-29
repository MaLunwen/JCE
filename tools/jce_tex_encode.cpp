/*
 * jce_tex_encode.cpp  GPU block-compression encoder for the asset cooker.
 *
 * Thin C ABI over bimg::imageEncodeFromRgba8 (bgfx's image library). Encodes an
 * RGBA8 image to a GPU block format (BC1/BC5/BC7 desktop, ASTC/ETC2 mobile) the
 * GPU consumes directly with no runtime decode. Built into jce_cook (links
 * bgfx::bimg_encode / bimg / bx).
 */
extern "C" {
#include <jce/resource/jce_asset_format.h>
}

#include <bimg/bimg.h>
#include <bimg/encode.h>
#include <bx/allocator.h>
#include <bx/error.h>
#include <bx/file.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mutex>

static bimg::TextureFormat::Enum map_fmt(int jce_fmt)
{
    switch (jce_fmt) {
    case JCEASSET_TEXFMT_BC1:        return bimg::TextureFormat::BC1;
    case JCEASSET_TEXFMT_BC3:        return bimg::TextureFormat::BC3;
    case JCEASSET_TEXFMT_BC5:        return bimg::TextureFormat::BC5;
    case JCEASSET_TEXFMT_BC7:        return bimg::TextureFormat::BC7;
    case JCEASSET_TEXFMT_ASTC_4x4:   return bimg::TextureFormat::ASTC4x4;
    case JCEASSET_TEXFMT_ETC2_RGBA8: return bimg::TextureFormat::ETC2A;
    default:                         return bimg::TextureFormat::Unknown;
    }
}

extern "C" int jce_tex_format_is_block(int jce_fmt)
{
    return map_fmt(jce_fmt) != bimg::TextureFormat::Unknown;
}

/* Decode one mip (block-compressed BC/ASTC/ETC, or already-RGBA8) into
 * RGBA8.  `dst` must hold w*h*4 bytes.  Used by the editor to PREVIEW
 * cooked .jceasset textures on the CPU: the runtime uploads cooked block
 * data straight to the GPU (jce_texture_from_cooked), but the editor's
 * texture cache is RGBA8-based, so it CPU-decodes the base mip here.
 * Returns 1 on success. */
extern "C" int jce_tex_decode_to_rgba8(const void *src, uint32_t w, uint32_t h,
                                       int jce_fmt, void *dst)
{
    if (!src || !dst || w == 0 || h == 0) return 0;
    if (jce_fmt == JCEASSET_TEXFMT_RGBA8) {
        memcpy(dst, src, (size_t)w * (size_t)h * 4u);
        return 1;
    }
    bimg::TextureFormat::Enum f = map_fmt(jce_fmt);
    if (f == bimg::TextureFormat::Unknown) return 0;
    static bx::DefaultAllocator s_alloc;
    bimg::imageDecodeToRgba8(&s_alloc, dst, src, w, h, w * 4u, f);
    return 1;
}

/* Size in bytes of one mip of (w,h) encoded to jce_fmt (block-rounded). */
extern "C" uint32_t jce_tex_encoded_size(uint32_t w, uint32_t h, int jce_fmt)
{
    bimg::TextureFormat::Enum f = map_fmt(jce_fmt);
    if (f == bimg::TextureFormat::Unknown) return 0;
    return bimg::imageGetSize(NULL, (uint16_t)w, (uint16_t)h, (uint16_t)1,
                              false, false, (uint16_t)1, f);
}

/* Map a generic cook quality (0=default, 1=fast, 2=highest) to the bimg
 * preset, in the colour or normal-map family.  Fast == squish range-fit,
 * which is several times quicker than the cluster-fit default at a modest
 * quality cost — the dominant lever for texture-cook wall time. */
static bimg::Quality::Enum pick_quality(int quality, int normal_map)
{
    if (normal_map) {
        switch (quality) {
        case 1:  return bimg::Quality::NormalMapFastest;
        case 2:  return bimg::Quality::NormalMapHighest;
        default: return bimg::Quality::NormalMapDefault;
        }
    }
    switch (quality) {
    case 1:  return bimg::Quality::Fastest;
    case 2:  return bimg::Quality::Highest;
    default: return bimg::Quality::Default;
    }
}

/* Encode an RGBA8 image into dst (must be >= jce_tex_encoded_size).
 * normal_map != 0 selects a normal-map-aware quality preset (for BC5).
 * quality: 0=default, 1=fast, 2=highest.  Returns 1 on success, 0 on failure.
 *
 * Thread-safety: the squish (BC1/BC3/BC5), ASTC, and ETC paths are fully
 * re-entrant (the shared s_alloc forwards to the thread-safe CRT allocator,
 * and each call uses only stack/dst buffers).  The BC7/BC6H nvtt path is
 * NOT — it writes process-global AVPCL/ZOH flags — so callers that fan
 * cooking across threads must serialise BC7/BC6H (see jce_bundle_pack). */
extern "C" int jce_tex_encode(const uint8_t *rgba, uint32_t w, uint32_t h,
                              int jce_fmt, int normal_map, int quality,
                              void *dst, uint32_t dst_size)
{
    bimg::TextureFormat::Enum f = map_fmt(jce_fmt);
    if (f == bimg::TextureFormat::Unknown || !rgba || !dst) return 0;

    uint32_t need = bimg::imageGetSize(NULL, (uint16_t)w, (uint16_t)h, (uint16_t)1,
                                       false, false, (uint16_t)1, f);
    if (need == 0 || need > dst_size) return 0;

    static bx::DefaultAllocator s_alloc;
    bx::Error err;
    bimg::Quality::Enum q = pick_quality(quality, normal_map);
    if (f == bimg::TextureFormat::BC7 || f == bimg::TextureFormat::BC6H) {
        /* imageEncodeFromRgba8 stubs out BC6H/BC7. The real encoder is nvtt,
         * reached via imageEncode (RGBA8 -> RGBA32F -> nvtt::compressBC7).
         * nvtt writes process-global AVPCL/ZOH compression flags, so concurrent
         * BC7/BC6H encodes corrupt each other — serialise this path under a
         * process-wide lock.  squish (BC1/3/5) / ASTC below are reentrant and
         * stay lock-free, so the default policy is unaffected. */
        static std::mutex s_nvtt_mutex;
        std::lock_guard<std::mutex> lk(s_nvtt_mutex);
        bimg::imageEncode(&s_alloc, dst, rgba, bimg::TextureFormat::RGBA8,
                          w, h, 1, f, q, &err);
    } else {
        bimg::imageEncodeFromRgba8(&s_alloc, dst, rgba, w, h, 1, f, q, &err);
    }
    return err.isOk() ? 1 : 0;
}

/*
 * Write an RGBA8 buffer as a PNG, through the encoder this TU already owns.
 *
 * NOT SDL_image: contracts/dependency-ownership.yml keeps the image-decode-ldr
 * capability at ONE directory and names "direct IMG_Load at call sites" as a
 * forbidden alternative, and the boundary gate refused the first version of
 * the atlas cook for including it.  bimg is already linked here for the block
 * encoder, so this adds no dependency at all -- it exposes one that was
 * already paid for.
 *
 * Used by tools/jce_cook_atlas.c (--pack-atlas).  Returns 0 on success.
 */
extern "C" int jce_tex_write_png(const char *path, const uint8_t *rgba,
                                 uint32_t w, uint32_t h)
{
    if (!path || !rgba || w == 0u || h == 0u) return 1;

    bx::FileWriter writer;
    bx::Error      err;
    if (!bx::open(&writer, path, false, &err)) return 2;

    /* Returns bytes written, not a bool -- 0 means it wrote nothing. */
    const int32_t written = bimg::imageWritePng(&writer, w, h, w * 4u, rgba,
                                                bimg::TextureFormat::RGBA8,
                                                false, &err);
    bx::close(&writer);
    return (written > 0 && err.isOk()) ? 0 : 3;
}
