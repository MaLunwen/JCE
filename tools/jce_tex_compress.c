/*
 * jce_tex_compress.c  Texture mipmap generation implementation.
 *
 * Provides box-filter mipmap generation for the asset cooker.
 */

#include "resource/jce_tex_compress.h"

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

void jce_tex_generate_mip(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                          uint8_t *dst, uint32_t *dst_w, uint32_t *dst_h)
{
    uint32_t w = (src_w > 1) ? src_w / 2 : 1;
    uint32_t h = (src_h > 1) ? src_h / 2 : 1;

    if (dst_w) *dst_w = w;
    if (dst_h) *dst_h = h;

    /* Box filter: average 2x2 pixels. */
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sx = x * 2;
            uint32_t sy = y * 2;

            for (int c = 0; c < 4; c++) {
                uint32_t sum = 0;
                sum += src[(sy * src_w + sx) * 4 + c];
                sum += src[(sy * src_w + sx + 1) * 4 + c];
                sum += src[((sy + 1) * src_w + sx) * 4 + c];
                sum += src[((sy + 1) * src_w + sx + 1) * 4 + c];
                dst[(y * w + x) * 4 + c] = (uint8_t)(sum / 4);
            }
        }
    }
}
