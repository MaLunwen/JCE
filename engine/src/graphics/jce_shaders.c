/* jce_shaders.c
 *
 * Runtime bgfx shader loading from a PAK archive.
 * Decompresses .bin blobs, feeds them to bgfx_create_shader().
 */

#include <jce/graphics/jce_shaders.h>
#include <jce/core/jce_log.h>

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
static bgfx_shader_handle_t load_single(const PakArchive *pak, const char *path)
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

/* Internal: load a program with explicit VS and FS base names.
   Skinned programs share the fragment shader with their non-skinned
   counterpart (e.g., vs_pbr_skinned + fs_pbr). */
static JceShaderHandle load_program_named(const PakArchive *pak,
                                          const char *vs_base,
                                          const char *fs_base)
{
    const char *sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) {
        LOG_ERROR(LOG_TAG, "no shader suffix for renderer %s",
                  bgfx_get_renderer_name(bgfx_get_renderer_type()));
        return JCE_INVALID_SHADER;
    }

    char vs_path[256], fs_path[256];
    snprintf(vs_path, sizeof(vs_path), "shaders/vs_%s_%s.bin", vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path), "shaders/fs_%s_%s.bin", fs_base, sfx);

    bgfx_shader_handle_t vsh = load_single(pak, vs_path);
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    bgfx_shader_handle_t fsh = load_single(pak, fs_path);
    if (fsh.idx == UINT16_MAX) {
        bgfx_destroy_shader(vsh);
        return JCE_INVALID_SHADER;
    }

    bgfx_program_handle_t prog = bgfx_create_program(vsh, fsh, true);
    return (JceShaderHandle){ prog.idx };
}

JceShaderHandle shader_load_program(const PakArchive *pak, const char *name)
{
    return load_program_named(pak, name, name);
}

JceShaderHandle shader_load_program_named(const PakArchive *pak,
                                          const char *vs_base,
                                          const char *fs_base)
{
    return load_program_named(pak, vs_base, fs_base);
}

JceShaderSet jce_shaders_load_all(const PakArchive *pak)
{
    JceShaderSet set;
    set.color    = shader_load_program(pak, "color");
    set.textured = shader_load_program(pak, "textured");
    set.mesh     = shader_load_program(pak, "mesh");
    set.pbr            = shader_load_program(pak, "pbr");
    /* Skinned variants share the fragment shader with their non-skinned
       counterpart: vs_pbr_skinned + fs_pbr, vs_shadow_skinned + fs_shadow. */
    set.pbr_skinned    = load_program_named(pak, "pbr_skinned",    "pbr");
    set.shadow         = shader_load_program(pak, "shadow");
    set.shadow_skinned = load_program_named(pak, "shadow_skinned", "shadow");

    if (!jce_shader_valid(set.color))
        LOG_ERROR(LOG_TAG, "failed to load 'color' shader");
    if (!jce_shader_valid(set.textured))
        LOG_WARN(LOG_TAG, "'textured' shader unavailable");
    if (!jce_shader_valid(set.mesh))
        LOG_WARN(LOG_TAG, "'mesh' shader unavailable");
    if (!jce_shader_valid(set.pbr))
        LOG_WARN(LOG_TAG, "'pbr' shader unavailable");
    if (!jce_shader_valid(set.pbr_skinned))
        LOG_WARN(LOG_TAG, "'pbr_skinned' shader unavailable");
    if (!jce_shader_valid(set.shadow))
        LOG_WARN(LOG_TAG, "'shadow' shader unavailable");
    if (!jce_shader_valid(set.shadow_skinned))
        LOG_WARN(LOG_TAG, "'shadow_skinned' shader unavailable");

    return set;
}
