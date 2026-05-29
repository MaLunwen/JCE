/* jce_shaders.c
 *
 * Runtime bgfx shader loading from a PAK archive.
 * Decompresses .bin blobs, feeds them to bgfx_create_shader().
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_shaders.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_shaders"

/* -- Embedded engine-shader PAK fallback ----------------------------- *
 * The engine static library bundles all stock shader bins (vs_color,
 * fs_pbr, …) for every backend (dx11, spv, glsl, essl, mtl).  When the
 * user PAK is NULL — or doesn't contain a particular shader — we fall
 * back to this embedded archive so a freshly-scaffolded SDK consumer
 * project still renders without shipping a separate engine_shaders.pak.
 *
 * The symbols `jce_engine_shaders_pak_data[]` /
 * `jce_engine_shaders_pak_size` are emitted at build time by
 * `tools/jce_pak --symbol-prefix jce_engine_shaders_pak`.
 *
 * When the engine is built without the embedded shader pack
 * (`-DJCE_EMBED_ENGINE_SHADERS=OFF`), a 1-byte stub keeps the link
 * happy and the fallback becomes a no-op.
 */
extern const unsigned char jce_engine_shaders_pak[];
extern const size_t        jce_engine_shaders_pak_size;

static const JcePakArchive *embedded_pak(void)
{
    static const JcePakArchive *cached = NULL;
    static int                  tried  = 0;
    if (!tried) {
        tried = 1;
        if (jce_engine_shaders_pak_size > 1) {
            cached = jce_pak_open(jce_engine_shaders_pak,
                                  jce_engine_shaders_pak_size);
            if (!cached) {
                LOG_WARN(LOG_TAG,
                         "embedded engine shader pak failed to open "
                         "(size=%zu)", (size_t)jce_engine_shaders_pak_size);
            }
        }
    }
    return cached;
}

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

/* Load one shader (.bin) from the PAK, return a bgfx handle.
   Falls back to the engine-embedded shader pak when the caller-supplied
   pak is NULL or the path is missing. */
static bgfx_shader_handle_t load_single(const JcePakArchive *pak, const char *path)
{
    bgfx_shader_handle_t invalid;
    invalid.idx = UINT16_MAX;

    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fb = embedded_pak();
        if (fb && fb != pak) {
            asset = jce_pak_find(fb, path);
            if (asset) pak = fb;
        }
    }
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found in PAK: %s", path);
        return invalid;
    }

    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return invalid;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", path);
        JCE_FREE(buf);
        return invalid;
    }

    /* bgfx_copy allocates internal memory and copies; we can free buf. */
    const bgfx_memory_t *mem =
        bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);

    return bgfx_create_shader(mem);
}

/* Internal: load a program with explicit VS and FS base names.
   Skinned programs share the fragment shader with their non-skinned
   counterpart (e.g., vs_pbr_skinned + fs_pbr). */
static JceShaderHandle load_program_named(const JcePakArchive *pak,
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

JceShaderHandle shader_load_program(const JcePakArchive *pak, const char *name)
{
    return load_program_named(pak, name, name);
}

JceShaderHandle shader_load_program_named(const JcePakArchive *pak,
                                          const char *vs_base,
                                          const char *fs_base)
{
    return load_program_named(pak, vs_base, fs_base);
}

/* ================================================================== */
/* Filesystem overlay (hot-reload support)                            */
/* ================================================================== */

static bgfx_shader_handle_t load_single_fs(const char *full_path)
{
    bgfx_shader_handle_t invalid;
    invalid.idx = UINT16_MAX;

    SDL_IOStream *io = SDL_IOFromFile(full_path, "rb");
    if (!io) {
        LOG_ERROR(LOG_TAG, "fs open failed: %s", full_path);
        return invalid;
    }

    Sint64 sz = SDL_GetIOSize(io);
    if (sz <= 0) {
        SDL_CloseIO(io);
        LOG_ERROR(LOG_TAG, "fs empty: %s", full_path);
        return invalid;
    }

    void *buf = JCE_MALLOC((size_t)sz);
    if (!buf) {
        SDL_CloseIO(io);
        return invalid;
    }

    size_t n = SDL_ReadIO(io, buf, (size_t)sz);
    SDL_CloseIO(io);

    if (n != (size_t)sz) {
        JCE_FREE(buf);
        LOG_ERROR(LOG_TAG, "fs short read: %s", full_path);
        return invalid;
    }

    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)sz);
    JCE_FREE(buf);
    return bgfx_create_shader(mem);
}

static JceShaderHandle load_program_fs_named(const char *dev_dir,
                                             const char *vs_base,
                                             const char *fs_base)
{
    if (!dev_dir || !vs_base || !fs_base) return JCE_INVALID_SHADER;

    const char *sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) return JCE_INVALID_SHADER;

    char vs_path[512], fs_path[512];
    snprintf(vs_path, sizeof(vs_path),
             "%s/shaders/vs_%s_%s.bin", dev_dir, vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path),
             "%s/shaders/fs_%s_%s.bin", dev_dir, fs_base, sfx);

    bgfx_shader_handle_t vsh = load_single_fs(vs_path);
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    bgfx_shader_handle_t fsh = load_single_fs(fs_path);
    if (fsh.idx == UINT16_MAX) {
        bgfx_destroy_shader(vsh);
        return JCE_INVALID_SHADER;
    }

    bgfx_program_handle_t prog = bgfx_create_program(vsh, fsh, true);
    return (JceShaderHandle){ prog.idx };
}

JceShaderHandle shader_load_program_fs(const char *dev_dir, const char *name)
{
    return load_program_fs_named(dev_dir, name, name);
}

JceShaderHandle shader_load_program_fs_named(const char *dev_dir,
                                             const char *vs_base,
                                             const char *fs_base)
{
    return load_program_fs_named(dev_dir, vs_base, fs_base);
}

JceShaderSet jce_shaders_load_all(const JcePakArchive *pak)
{
    JCE_PROFILE_ZONE_N("Shaders::LoadAll");
    JceShaderSet set;
    set.color    = shader_load_program(pak, "color");
    set.textured = shader_load_program(pak, "textured");
    set.mesh     = shader_load_program(pak, "mesh");
    set.pbr            = shader_load_program(pak, "pbr");
    /* Instanced PBR variant: vs_pbr_inst + fs_pbr (fragment unchanged). */
    set.pbr_inst       = load_program_named(pak, "pbr_inst",       "pbr");
    /* Skinned variants share the fragment shader with their non-skinned
       counterpart: vs_pbr_skinned + fs_pbr, vs_shadow_skinned + fs_shadow. */
    set.pbr_skinned    = load_program_named(pak, "pbr_skinned",    "pbr");
    set.shadow         = shader_load_program(pak, "shadow");
    /* Instanced shadow variant: vs_shadow_inst + fs_shadow (fragment unchanged). */
    set.shadow_inst    = load_program_named(pak, "shadow_inst",    "shadow");
    set.shadow_skinned = load_program_named(pak, "shadow_skinned", "shadow");
    /* VSM variant: vs_shadow_vsm + fs_shadow_vsm (writes (z, z^2) moments). */
    set.shadow_vsm     = shader_load_program(pak, "shadow_vsm");
    set.terrain        = shader_load_program(pak, "terrain");

    if (!jce_shader_valid(set.color))
        LOG_ERROR(LOG_TAG, "failed to load 'color' shader");
    if (!jce_shader_valid(set.textured))
        LOG_WARN(LOG_TAG, "'textured' shader unavailable");
    if (!jce_shader_valid(set.mesh))
        LOG_WARN(LOG_TAG, "'mesh' shader unavailable");
    if (!jce_shader_valid(set.pbr))
        LOG_WARN(LOG_TAG, "'pbr' shader unavailable");
    if (!jce_shader_valid(set.pbr_inst))
        LOG_WARN(LOG_TAG, "'pbr_inst' shader unavailable (GPU instancing disabled)");
    if (!jce_shader_valid(set.pbr_skinned))
        LOG_WARN(LOG_TAG, "'pbr_skinned' shader unavailable");
    if (!jce_shader_valid(set.shadow))
        LOG_WARN(LOG_TAG, "'shadow' shader unavailable");
    if (!jce_shader_valid(set.shadow_inst))
        LOG_WARN(LOG_TAG, "'shadow_inst' shader unavailable (shadow instancing disabled)");
    if (!jce_shader_valid(set.shadow_skinned))
        LOG_WARN(LOG_TAG, "'shadow_skinned' shader unavailable");
    if (!jce_shader_valid(set.shadow_vsm))
        LOG_WARN(LOG_TAG, "'shadow_vsm' shader unavailable (VSM mode disabled)");
    if (!jce_shader_valid(set.terrain))
        LOG_WARN(LOG_TAG, "'terrain' shader unavailable");

    JCE_PROFILE_ZONE_END;
    return set;
}

/* ================================================================== */
/* FS-overlay variant of jce_shaders_load_all                         */
/* ================================================================== */

/* Try FS first, fall back to PAK on a per-shader basis. */
static JceShaderHandle load_overlay(const char *dev_dir,
                                    const JcePakArchive *pak,
                                    const char *vs, const char *fs)
{
    JceShaderHandle h = JCE_INVALID_SHADER;
    if (dev_dir && dev_dir[0])
        h = load_program_fs_named(dev_dir, vs, fs);
    if (!jce_shader_valid(h))
        h = load_program_named(pak, vs, fs);
    return h;
}

JceShaderSet jce_shaders_load_all_fs(const char *dev_dir,
                                     const JcePakArchive *pak)
{
    JCE_PROFILE_ZONE_N("Shaders::LoadAllFS");
    JceShaderSet set;
    set.color          = load_overlay(dev_dir, pak, "color",          "color");
    set.textured       = load_overlay(dev_dir, pak, "textured",       "textured");
    set.mesh           = load_overlay(dev_dir, pak, "mesh",           "mesh");
    set.pbr            = load_overlay(dev_dir, pak, "pbr",            "pbr");
    set.pbr_inst       = load_overlay(dev_dir, pak, "pbr_inst",       "pbr");
    set.pbr_skinned    = load_overlay(dev_dir, pak, "pbr_skinned",    "pbr");
    set.shadow         = load_overlay(dev_dir, pak, "shadow",         "shadow");
    set.shadow_inst    = load_overlay(dev_dir, pak, "shadow_inst",    "shadow");
    set.shadow_skinned = load_overlay(dev_dir, pak, "shadow_skinned", "shadow");
    set.shadow_vsm     = load_overlay(dev_dir, pak, "shadow_vsm",     "shadow_vsm");
    set.terrain        = load_overlay(dev_dir, pak, "terrain",        "terrain");
    JCE_PROFILE_ZONE_END;
    return set;
}
