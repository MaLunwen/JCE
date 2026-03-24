/* jce_shaders.h
 *
 * Runtime shader loading from a PAK archive.
 *
 * Usage:
 *   PakArchive *pak = pak_open(assets_pak_data, assets_pak_data_size);
 *   JceShaderHandle prog = shader_load_program(pak, "color");
 *   // loads shaders/vs_color_<backend>.bin + shaders/fs_color_<backend>.bin
 */
#ifndef JCE_SHADERS_H
#define JCE_SHADERS_H

#include "jce_gfx_types.h"
#include "resource/pak_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load a vertex+fragment shader pair from the PAK and
 * create a program.
 *
 * name: base name without vs_/fs_ prefix (e.g. "color").
 * Selects the correct backend suffix (dx11/spv/glsl/essl)
 * based on the active renderer.
 *
 * Returns valid handle or JCE_INVALID_SHADER on failure. */
JceShaderHandle shader_load_program(
    const PakArchive *pak, const char *name);

/* Pre-loaded shader set (color + textured + mesh). */
typedef struct JceShaderSet {
    JceShaderHandle color;
    JceShaderHandle textured;
    JceShaderHandle mesh;
} JceShaderSet;

/* Load all three standard shader programs from PAK.
 * Must be called after bgfx is initialized. */
JceShaderSet jce_shaders_load_all(const PakArchive *pak);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADERS_H */
