#ifndef JCE_WATER_GRADIENT_H
#define JCE_WATER_GRADIENT_H
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

/* RG: first cascade slopes, BA: optional second cascade slopes. Keep each
 * cascade independent: its UV period need not match the other cascade. */
static inline bool jce_water_gradient_pack(float *out, size_t capacity, int n,
    const float *sx, const float *sz, const float *sx2, const float *sz2)
{
    if (!out || !sx || !sz || n < 2 || n > 1024 || (n & (n - 1)) ||
        capacity < (size_t)n * (size_t)n * 4u || (!!sx2 != !!sz2)) return false;
    const size_t cells = (size_t)n * (size_t)n;
    for (size_t i = 0; i < cells; ++i) {
        if (!isfinite(sx[i]) || !isfinite(sz[i]) ||
            fabsf(sx[i]) > 65504.0f || fabsf(sz[i]) > 65504.0f ||
            (sx2 && (!isfinite(sx2[i]) || !isfinite(sz2[i]) ||
                fabsf(sx2[i]) > 65504.0f || fabsf(sz2[i]) > 65504.0f))) return false;
    }
    for (size_t i = 0; i < cells; ++i) {
        out[4u*i] = sx[i]; out[4u*i+1u] = sz[i];
        out[4u*i+2u] = sx2 ? sx2[i] : 0.0f;
        out[4u*i+3u] = sz2 ? sz2[i] : 0.0f;
    }
    return true;
}

/* In-place box reduction. Read each 2x2 source footprint before publishing
 * its destination; ascending rows never overwrite an unread source cell. */
static inline bool jce_water_gradient_reduce(float *data, size_t capacity, int n)
{
    if (!data || n < 2 || n > 1024 || (n & (n - 1)) ||
        capacity < (size_t)n * (size_t)n * 4u) return false;
    const int next = n / 2;
    for (int y = 0; y < next; ++y) for (int x = 0; x < next; ++x)
        for (int c = 0; c < 4; ++c) {
            const size_t i = ((size_t)(2*y) * (size_t)n + (size_t)(2*x)) * 4u + (size_t)c;
            data[((size_t)y * (size_t)next + (size_t)x) * 4u + (size_t)c] =
                0.25f * (data[i] + data[i+4u] + data[i+(size_t)n*4u] +
                         data[i+(size_t)n*4u+4u]);
        }
    return true;
}
#endif
