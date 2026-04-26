/*
 * jce_image.c  Public HDR decoding API; wraps the engine's vendored
 * stb_image build (HDR-only configuration).
 */

#include <jce/renderer/jce_image.h>

#include "internal/stb_image.h"

float *jce_image_load_hdr_from_memory(const void *data, uint64_t size, int *out_w, int *out_h)
{
    if (!data || size == 0)
        return NULL;
    int w = 0, h = 0, ch = 0;
    float *pixels = stbi_loadf_from_memory((const stbi_uc *)data, (int)size, &w, &h, &ch, 4);
    if (!pixels)
        return NULL;
    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    return pixels;
}

void jce_image_free_hdr(float *pixels)
{
    if (pixels)
        stbi_image_free(pixels);
}
