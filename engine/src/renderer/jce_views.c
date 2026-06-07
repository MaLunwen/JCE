/*
 * jce_views.c -- helpers for render-target creation.
 *
 * jce_clear_freshly_created_fbo() is intentionally a no-op kept for
 * ABI compatibility — view-id-based ordering cannot guarantee a clear
 * runs before the consumer pass on the same frame.
 *
 * jce_zero_init_mem() is the real workhorse: it returns a bgfx_memory_t
 * containing zeroed bytes that callers pass to bgfx_create_texture_2d's
 * _mem parameter. The texture's GPU storage is initialised to all-zero
 * AS PART OF the create command, so the very first sample sees solid
 * black instead of leftover VRAM (the "rainbow garbage" artefact).
 */

#include <jce/renderer/jce_views.h>

#include <bgfx/c99/bgfx.h>
#include <string.h>

void jce_clear_freshly_created_fbo(unsigned short fb_idx,
                                   unsigned short width,
                                   unsigned short height)
{
    (void)fb_idx;
    (void)width;
    (void)height;
}

const struct bgfx_memory_s *jce_zero_init_mem(unsigned int size_bytes)
{
    if (size_bytes == 0) return NULL;
    const bgfx_memory_t *mem = bgfx_alloc(size_bytes);
    if (!mem || !mem->data) return NULL;
    memset(mem->data, 0, mem->size);
    return mem;
}
