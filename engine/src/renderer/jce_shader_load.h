/*
 * jce_shader_load.h  Shared bgfx shader-binary loading for effect modules.
 *
 * Every subsystem that loads its own shader blobs (SSAO, SSR, volumetric
 * fog, decals, GPU particles, GPU scene, GI probes, weather) resolves the
 * same "shaders/<name>_<backend>.bin" PAK key, and every one of them MUST
 * fall back to the embedded engine-shader PAK when the caller's PAK does
 * not carry it (scene/editor paks ship zero shaders; the stock shader bins
 * are baked into jce_renderer).  This header is the single source of truth
 * for both steps.
 *
 * Header-only (static inline) so no extra translation unit or link edge is
 * needed.  It lives under renderer/ on purpose: middleware may include
 * renderer, never the other way round.
 */

#ifndef JCE_SHADER_LOAD_H
#define JCE_SHADER_LOAD_H

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/resource/jce_pak_loader.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shader-binary suffix for the live bgfx backend; NULL when unsupported. */
static inline const char *jce_shader_backend_suffix(void)
{
    switch (bgfx_get_renderer_type()) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12: return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:     return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:     return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:   return "essl";
    case BGFX_RENDERER_TYPE_METAL:      return "mtl";
    default:                            return NULL;
    }
}

/*
 * Load "shaders/<name>_<sfx>.bin" from `pak`, falling back to the embedded
 * engine-shader pak when `pak` is NULL or does not carry the blob.
 * `log_tag` is the caller's LOG_TAG.  Returns an invalid handle
 * (idx == UINT16_MAX) on any failure.
 */
static inline bgfx_shader_handle_t jce_shader_load_from_pak(const JcePakArchive *pak,
                                                            const char *name,
                                                            const char *sfx,
                                                            const char *log_tag)
{
    bgfx_shader_handle_t invalid = { UINT16_MAX };
    char path[256];
    snprintf(path, sizeof(path), "shaders/%s_%s.bin", name, sfx);

    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fb = jce_shaders_embedded_engine_pak();
        if (fb && fb != pak) asset = jce_pak_find(fb, path);
    }
    if (!asset) {
        LOG_ERROR(log_tag, "shader not in pak: %s", path);
        return invalid;
    }
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return invalid;
    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) { JCE_FREE(buf); return invalid; }
    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);
    return bgfx_create_shader(mem);
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADER_LOAD_H */
