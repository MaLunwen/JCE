/*
 * jce_tex_compress.c  Texture mipmap generation implementation.
 *
 * Provides box-filter mipmap generation for the asset cooker.
 */

#include "resource/jce_tex_compress.h"

#include <jce/resource/jce_asset_format.h>

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
        /* Block-compressed formats: block-rounded size via the encoder. */
        default:                    s = jce_tex_encoded_size(mw, mh, jce_fmt); break;
        }
        if (s == 0) return 0;                  /* unsupported format */
        total += s;
        if (total > 0xFFFFFFFFu) return 0;     /* 32-bit overflow guard */
    }
    return (uint32_t)total;
}

void jce_tex_generate_mip(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                          uint8_t *dst, uint32_t *dst_w, uint32_t *dst_h)
{
    uint32_t w = (src_w > 1) ? src_w / 2 : 1;
    uint32_t h = (src_h > 1) ? src_h / 2 : 1;

    if (dst_w) *dst_w = w;
    if (dst_h) *dst_h = h;

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

            for (int c = 0; c < 4; c++) {
                uint32_t sum = 0;
                sum += src[(sy0 * src_w + sx0) * 4 + c];
                sum += src[(sy0 * src_w + sx1) * 4 + c];
                sum += src[(sy1 * src_w + sx0) * 4 + c];
                sum += src[(sy1 * src_w + sx1) * 4 + c];
                dst[(y * w + x) * 4 + c] = (uint8_t)(sum / 4);
            }
        }
    }
}
