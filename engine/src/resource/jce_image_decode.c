/*
 * jce_image_decode.c  Generic image decode wrapper (stb_image backend).
 */

#include <jce/resource/jce_image_decode.h>
#include <jce/os/core/jce_filesystem.h>

#include "os/core/jce_memory.h"

#include <stdlib.h>  /* free() — stb_image_free is just free in our config */
#include <string.h>

/* stb_image is implemented in renderer/jce_stb_image_impl.c with the
   STBI_MALLOC/FREE macros pointing at the engine allocator.  We only
   need the public function declarations here. */
#define STBI_NO_STDIO       /* match the impl translation unit */
#include "renderer/internal/stb_image.h"

bool jce_image_decode(const void *data, size_t size, JceImage *out)
{
    if (!data || size == 0 || !out) return false;

    int w = 0, h = 0, n = 0;
    stbi_uc *px = stbi_load_from_memory((const stbi_uc *)data,
                                        (int)size, &w, &h, &n,
                                        /*req_comp=*/4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }

    /* Copy into engine-owned buffer so callers free with JCE_FREE
       (stb_image_free maps to the engine allocator via the macros in
       jce_stb_image_impl.c, but we hide that contract from clients). */
    size_t bytes = (size_t)w * (size_t)h * 4u;
    uint8_t *copy = (uint8_t *)JCE_MALLOC(bytes);
    if (!copy) {
        stbi_image_free(px);
        return false;
    }
    memcpy(copy, px, bytes);
    stbi_image_free(px);

    out->pixels = copy;
    out->width  = (uint32_t)w;
    out->height = (uint32_t)h;
    return true;
}

bool jce_image_decode_file(const char *path, JceImage *out)
{
    if (!path || !out) return false;
    size_t sz = 0;
    void *buf = jce_fs_host_read_all(path, &sz);
    if (!buf) return false;
    bool ok = jce_image_decode(buf, sz, out);
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
