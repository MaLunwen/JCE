/* Pure RGBA8 mip construction. No GPU, global policy or allocation. */
#ifndef JCE_TEXTURE_MIPS_H
#define JCE_TEXTURE_MIPS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>

static inline bool jce_texture_mip_bytes(uint32_t w, uint32_t h, size_t *out)
{
    uint64_t total = 0;
    if (!w || !h || !out) return false;
    for (;;) {
        uint64_t pixels = (uint64_t)w * h;
        if (pixels > UINT32_MAX / 4u) return false;
        uint64_t bytes = pixels * 4u;
        if (total > UINT32_MAX - bytes) return false;
        total += bytes;
        if (w == 1 && h == 1) break;
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
    }
    *out = (size_t)total;
    return true;
}

static inline void jce_texture_mip_downsample(const uint8_t *src,
    uint32_t sw, uint32_t sh, uint8_t *dst)
{
    uint32_t dw = sw > 1 ? sw >> 1 : 1;
    uint32_t dh = sh > 1 ? sh >> 1 : 1;
    for (uint32_t y = 0; y < dh; ++y) {
        uint32_t y0 = y * 2u, y1 = y0 + 1u < sh ? y0 + 1u : y0;
        for (uint32_t x = 0; x < dw; ++x) {
            uint32_t x0 = x * 2u, x1 = x0 + 1u < sw ? x0 + 1u : x0;
            const uint8_t *a = src + ((size_t)y0 * sw + x0) * 4u;
            const uint8_t *b = src + ((size_t)y0 * sw + x1) * 4u;
            const uint8_t *c = src + ((size_t)y1 * sw + x0) * 4u;
            const uint8_t *d = src + ((size_t)y1 * sw + x1) * 4u;
            uint8_t *pixel = dst + ((size_t)y * dw + x) * 4u;
            for (int channel = 0; channel < 4; ++channel)
                pixel[channel] = (uint8_t)(((unsigned)a[channel] + b[channel]
                    + c[channel] + d[channel]) >> 2);
        }
    }
}

/* Base pixels are already populated; writes only subsequent levels. */
static inline bool jce_texture_mip_fill(uint8_t *chain, size_t bytes,
    uint32_t w, uint32_t h)
{
    size_t needed;
    if (!chain || !jce_texture_mip_bytes(w, h, &needed) || bytes < needed) return false;
    uint8_t *previous = chain;
    uint8_t *next = chain + (size_t)w * h * 4u;
    while (w > 1 || h > 1) {
        jce_texture_mip_downsample(previous, w, h, next);
        w = w > 1 ? w >> 1 : 1;
        h = h > 1 ? h >> 1 : 1;
        previous = next;
        next += (size_t)w * h * 4u;
    }
    return true;
}

/* Select existing mip suffix; the caller owns policy and all source bytes. */
static inline size_t jce_texture_mip_select(uint32_t *w, uint32_t *h, uint32_t cap)
{
    size_t offset = 0;
    while (cap && (*w > cap || *h > cap)) {
        offset += (size_t)*w * *h * 4u;
        *w = *w > 1 ? *w >> 1 : 1;
        *h = *h > 1 ? *h >> 1 : 1;
    }
    return offset;
}

#endif /* JCE_TEXTURE_MIPS_H */
