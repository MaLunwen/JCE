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

#include <stdint.h>
#include <stdio.h>

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

/* Size in bytes of one mip of (w,h) encoded to jce_fmt (block-rounded). */
extern "C" uint32_t jce_tex_encoded_size(uint32_t w, uint32_t h, int jce_fmt)
{
    bimg::TextureFormat::Enum f = map_fmt(jce_fmt);
    if (f == bimg::TextureFormat::Unknown) return 0;
    return bimg::imageGetSize(NULL, (uint16_t)w, (uint16_t)h, (uint16_t)1,
                              false, false, (uint16_t)1, f);
}

/* Encode an RGBA8 image into dst (must be >= jce_tex_encoded_size).
 * normal_map != 0 selects a normal-map-aware quality preset (for BC5).
 * Returns 1 on success, 0 on failure. */
extern "C" int jce_tex_encode(const uint8_t *rgba, uint32_t w, uint32_t h,
                              int jce_fmt, int normal_map,
                              void *dst, uint32_t dst_size)
{
    bimg::TextureFormat::Enum f = map_fmt(jce_fmt);
    if (f == bimg::TextureFormat::Unknown || !rgba || !dst) return 0;

    uint32_t need = bimg::imageGetSize(NULL, (uint16_t)w, (uint16_t)h, (uint16_t)1,
                                       false, false, (uint16_t)1, f);
    if (need == 0 || need > dst_size) return 0;

    static bx::DefaultAllocator s_alloc;
    bx::Error err;
    bimg::Quality::Enum q = normal_map ? bimg::Quality::NormalMapDefault
                                       : bimg::Quality::Default;
    if (f == bimg::TextureFormat::BC7 || f == bimg::TextureFormat::BC6H) {
        /* imageEncodeFromRgba8 stubs out BC6H/BC7. The real encoder is nvtt,
         * reached via imageEncode (RGBA8 -> RGBA32F -> nvtt::compressBC7). */
        bimg::imageEncode(&s_alloc, dst, rgba, bimg::TextureFormat::RGBA8,
                          w, h, 1, f, q, &err);
    } else {
        bimg::imageEncodeFromRgba8(&s_alloc, dst, rgba, w, h, 1, f, q, &err);
    }
    return err.isOk() ? 1 : 0;
}
