/*
 * jce_tex_compress.c  Texture mipmap generation implementation.
 *
 * Provides box-filter mipmap generation for the asset cooker.
 */

#include "resource/jce_tex_compress.h"

#include <jce/resource/jce_asset_format.h>

/* math.h is used by the sRGB decode/encode below.  Named explicitly rather
 * than relied upon transitively: without a declaration, C99 gives `pow` an
 * implicit int return and every sRGB value comes out wrong -- and the unit
 * tests still PASSED here on a transitive include, which is exactly how that
 * would have shipped unnoticed to a toolchain whose headers differ. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* ================================================================== */
/* Mipmap generation                                                   */
/* ================================================================== */

uint32_t jce_tex_mip_count(uint32_t width, uint32_t height)
{
    uint32_t count = 1;
    uint32_t w = width, h = height;
    while (w > 1 || h > 1) {
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
        count++;
    }
    return count;
}

void jce_tex_mip_dimensions(uint32_t base_w, uint32_t base_h,
                            uint32_t mip_level,
                            uint32_t *out_w, uint32_t *out_h)
{
    uint32_t w = base_w, h = base_h;
    for (uint32_t i = 0; i < mip_level; i++) {
        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

uint32_t jce_tex_cooked_pixel_size(uint32_t w, uint32_t h, int jce_fmt,
                                   uint32_t mip_count)
{
    if (w == 0 || h == 0) return 0;
    if (mip_count == 0) mip_count = 1;
    /* A file claiming more mips than the dimensions allow is malformed. */
    if (mip_count > jce_tex_mip_count(w, h)) return 0;

    uint64_t total = 0;
    for (uint32_t m = 0; m < mip_count; m++) {
        uint32_t mw, mh;
        jce_tex_mip_dimensions(w, h, m, &mw, &mh);
        /* area never overflows uint64 ((2^32-1)^2 < UINT64_MAX), but the
         * *bytes-per-pixel multiply can — guard it before computing `s` so a
         * crafted near-uint32-max w/h cannot wrap a per-mip term down past the
         * 32-bit total guard below (audit Round-3 P2). */
        uint64_t area = (uint64_t)mw * mh;
        uint64_t s;
        switch (jce_fmt) {
        /* Uncompressed formats are stored raw (bimg's encoder only knows the
         * block formats, so jce_tex_encoded_size returns 0 for these). */
        case JCEASSET_TEXFMT_RGBA8:
            if (area > 0xFFFFFFFFull / 4u) return 0;
            s = area * 4u; break;
        case JCEASSET_TEXFMT_RGB8:
            if (area > 0xFFFFFFFFull / 3u) return 0;
            s = area * 3u; break;
        case JCEASSET_TEXFMT_R16F:
            if (area > 0xFFFFFFFFull / 2u) return 0;
            s = area * 2u; break;
        case JCEASSET_TEXFMT_RG16F:
        case JCEASSET_TEXFMT_R32F:
            if (area > 0xFFFFFFFFull / 4u) return 0;
            s = area * 4u; break;
        case JCEASSET_TEXFMT_RGBA16F:
        case JCEASSET_TEXFMT_RG32F:
            if (area > 0xFFFFFFFFull / 8u) return 0;
            s = area * 8u; break;
        case JCEASSET_TEXFMT_RGBA32F:
            if (area > 0xFFFFFFFFull / 16u) return 0;
            s = area * 16u; break;
        /* Block-compressed formats: block-rounded size via the encoder. */
        default:                    s = jce_tex_encoded_size(mw, mh, jce_fmt); break;
        }
        if (s == 0) return 0;                  /* unsupported format */
        total += s;
        if (total > 0xFFFFFFFFu) return 0;     /* 32-bit overflow guard */
    }
    return (uint32_t)total;
}

/* sRGB <-> linear, exact piecewise (not the pow(x,2.2) approximation the
 * shaders use).  The decode is a 256-entry table because the input is a byte
 * and the alternative is four powf() calls per texel per channel per mip. */
static const float *srgb_decode_table(void)
{
    static float s_lut[256];
    static int   s_ready = 0;
    if (!s_ready) {
        for (int i = 0; i < 256; ++i) {
            const double x = (double)i / 255.0;
            s_lut[i] = (float)(x <= 0.04045 ? x / 12.92
                                            : pow((x + 0.055) / 1.055, 2.4));
        }
        s_ready = 1;
    }
    return s_lut;
}

static uint8_t srgb_encode_byte(float lin)
{
    double x = (double)lin;
    if (!(x > 0.0)) x = 0.0;            /* also catches NaN */
    if (x > 1.0)    x = 1.0;
    const double y = (x <= 0.0031308) ? x * 12.92
                                      : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    const double b = y * 255.0 + 0.5;
    return (uint8_t)(b > 255.0 ? 255.0 : b);
}

/* See jce_tex_compress.h.
 *
 * `srgb` decides the SPACE the average is taken in, and it is the whole point
 * of this function existing separately from the legacy entry point below.
 *
 * A box filter over raw sRGB bytes is exactly right wherever the source is
 * flat and increasingly wrong as local contrast rises: a 2x2 of {255,255,0,0}
 * averages to 127 in gamma space and to 188 in linear light, a 61-level error,
 * and it is the linear answer a viewer perceives as half-lit.  Measured over
 * this repository's textures before the change: worst per-texel error 73
 * levels, median texture's worst 27, and on a leaf atlas the SIGNED mean shift
 * reaches -14.5 luma / +16 R-B by mip 7 -- distant foliage rendered darker and
 * warmer than it should be, by an amount that grows with mip level, which is
 * to say with distance.
 *
 * Alpha is averaged linearly in BOTH modes.  sRGB describes the colour
 * channels; alpha is coverage.  Decoding it would move alpha-tested foliage
 * coverage at every mip, thinning or thickening a canopy with distance, which
 * is a worse artifact than the one being fixed. */
void jce_tex_generate_mip_ex(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                             uint8_t *dst, uint32_t *dst_w, uint32_t *dst_h,
                             bool srgb)
{
    uint32_t w = (src_w > 1) ? src_w / 2 : 1;
    uint32_t h = (src_h > 1) ? src_h / 2 : 1;

    if (dst_w) *dst_w = w;
    if (dst_h) *dst_h = h;

    const float *dec = srgb ? srgb_decode_table() : NULL;

    /* Box filter: average a 2x2 source footprint.  Clamp the +1 sample to
     * the last valid row/column.  Every non-square mip chain reaches an
     * N×1 / 1×N level before the final 1×1 (e.g. 1024×512 -> ... -> 2×1):
     * at src_h==1 or src_w==1 the (sy+1)/(sx+1) neighbour does not exist,
     * and reading it walked off the buffer end and corrupted the heap — an
     * access violation that only fired for non-square textures. */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sx0 = x * 2;
            uint32_t sy0 = y * 2;
            uint32_t sx1 = (sx0 + 1 < src_w) ? sx0 + 1 : sx0;
            uint32_t sy1 = (sy0 + 1 < src_h) ? sy0 + 1 : sy0;

            const uint8_t *p00 = src + (size_t)(sy0 * src_w + sx0) * 4;
            const uint8_t *p01 = src + (size_t)(sy0 * src_w + sx1) * 4;
            const uint8_t *p10 = src + (size_t)(sy1 * src_w + sx0) * 4;
            const uint8_t *p11 = src + (size_t)(sy1 * src_w + sx1) * 4;
            uint8_t *o = dst + (size_t)(y * w + x) * 4;

            for (int c = 0; c < 3; c++) {
                if (dec) {
                    const float lin = 0.25f * (dec[p00[c]] + dec[p01[c]] +
                                               dec[p10[c]] + dec[p11[c]]);
                    o[c] = srgb_encode_byte(lin);
                } else {
                    const uint32_t sum = (uint32_t)p00[c] + p01[c] + p10[c] + p11[c];
                    o[c] = (uint8_t)(sum / 4u);
                }
            }
            /* Alpha: linear in both modes. */
            const uint32_t a = (uint32_t)p00[3] + p01[3] + p10[3] + p11[3];
            o[3] = (uint8_t)(a / 4u);
        }
    }
}

/* Legacy entry point: RAW averaging, unchanged.
 *
 * Deliberately not redefined to mean sRGB.  Callers that have not been given a
 * semantic keep exactly the behaviour they had, so nothing can start
 * gamma-decoding a normal map merely because this function grew an option. */
void jce_tex_generate_mip(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                          uint8_t *dst, uint32_t *dst_w, uint32_t *dst_h)
{
    jce_tex_generate_mip_ex(src, src_w, src_h, dst, dst_w, dst_h, false);
}

