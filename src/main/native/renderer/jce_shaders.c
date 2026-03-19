/* jce_shaders.c
 *
 * Runtime bgfx shader loading from a PAK archive.
 * Decompresses .bin blobs, feeds them to bgfx_create_shader().
 */

#include "jce_shaders.h"
#include "core/jce_log.h"

#include <SDL3/SDL.h>
#include <bgfx/c99/bgfx.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "jce_shaders"

/* Map bgfx renderer type to the file suffix produced by shaderc. */
static const char *shader_suffix(bgfx_renderer_type_t type)
{
    switch (type) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12:
        return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:
        return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:
        return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:
        return "essl";
    case BGFX_RENDERER_TYPE_METAL:
        return "mtl";
    default:
        return NULL;
    }
}

/* Load one shader (.bin) from the PAK, return a bgfx handle. */
static bgfx_shader_handle_t load_single(PakArchive *pak, const char *path)
{
    bgfx_shader_handle_t invalid;
    invalid.idx = UINT16_MAX;

    const PakAsset *asset = pak_find(pak, path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found in PAK: %s", path);
        return invalid;
    }

    void *buf = malloc((size_t)asset->original_size);
    if (!buf) return invalid;

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", path);
        free(buf);
        return invalid;
    }

    /* bgfx_copy allocates internal memory and copies; we can free buf. */
    const bgfx_memory_t *mem =
        bgfx_copy(buf, (uint32_t)asset->original_size);
    free(buf);

    return bgfx_create_shader(mem);
}

bgfx_program_handle_t shader_load_program(PakArchive *pak, const char *name)
{
    bgfx_program_handle_t invalid;
    invalid.idx = UINT16_MAX;

    const char *sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) {
        LOG_ERROR(LOG_TAG, "no shader suffix for renderer %s",
                  bgfx_get_renderer_name(bgfx_get_renderer_type()));
        return invalid;
    }

    char vs_path[256], fs_path[256];
    snprintf(vs_path, sizeof(vs_path), "shaders/vs_%s_%s.bin", name, sfx);
    snprintf(fs_path, sizeof(fs_path), "shaders/fs_%s_%s.bin", name, sfx);

    bgfx_shader_handle_t vsh = load_single(pak, vs_path);
    if (vsh.idx == UINT16_MAX) return invalid;

    bgfx_shader_handle_t fsh = load_single(pak, fs_path);
    if (fsh.idx == UINT16_MAX) {
        bgfx_destroy_shader(vsh);
        return invalid;
    }

    return bgfx_create_program(vsh, fsh, true);
}
