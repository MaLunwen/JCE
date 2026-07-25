/*
 * jce_image_decode.c  JceImage-shaped adapter over the one image service.
 *
 * This file does NOT pick a codec.  It forwards to jce_image, which is the
 * single dispatcher (LDR -> SDL_image, HDR / 16-bit grayscale -> stb_image),
 * so the editor preview, the density-mask loader, the PAK decode helper and
 * the cooker all see exactly the pixels the runtime texture loader sees.
 * It used to call stb_image directly, which made it a second general-purpose
 * loader with its own format set and its own JPEG IDCT — the thing the
 * dependency charter (README "Rendering, Images, Fonts, UI, and Assets")
 * explicitly forbids.
 */

#include <jce/resource/jce_image_decode.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_image.h>    /* the one image-decode service */

#include "os/core/jce_memory.h"

#include <string.h>

bool jce_image_decode(const void *data, size_t size, JceImage *out)
{
    if (!data || size == 0 || !out) return false;

    /* The service hands back a tightly-packed RGBA8 buffer owned by the
       ENGINE allocator, which is exactly what jce_image_free releases —
       so the buffer is adopted, not re-copied. */
    int w = 0, h = 0;
    uint8_t *px = jce_image_load_rgba8_from_memory(data, (uint64_t)size, &w, &h);
    if (!px) return false;
    if (w <= 0 || h <= 0) {
        jce_image_free_rgba8(px);
        return false;
    }

    out->pixels = px;
    out->width  = (uint32_t)w;
    out->height = (uint32_t)h;
    return true;
}

bool jce_image_decode_file(const char *path, JceImage *out)
{
    if (!path || !out) return false;
    uint64_t sz = 0;
    void *buf = jce_fs_host_read_all(path, &sz);
    if (!buf) return false;
    bool ok = jce_image_decode(buf, (size_t)sz, out);
    JCE_FREE(buf);
    return ok;
}

bool jce_image_create_blank(uint32_t width, uint32_t height, JceImage *out)
{
    if (!out || width == 0 || height == 0) return false;
    size_t bytes = (size_t)width * (size_t)height * 4u;
    uint8_t *px = (uint8_t *)JCE_MALLOC(bytes);
    if (!px) return false;
    memset(px, 0, bytes);
    out->pixels = px;
    out->width  = width;
    out->height = height;
    return true;
}

void jce_image_free(JceImage *img)
{
    if (!img) return;
    if (img->pixels) JCE_FREE(img->pixels);
    img->pixels = NULL;
    img->width  = 0;
    img->height = 0;
}
