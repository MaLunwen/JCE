/* jce_shaders.h
 *
 * Runtime shader loading from a PAK archive.
 *
 * Usage:
 *   PakArchive *pak = pak_open(assets_pak_data, assets_pak_data_size);
 *   JceShaderHandle prog = shader_load_program(pak, "color");
 *   // loads shaders/vs_color_<backend>.bin + shaders/fs_color_<backend>.bin
 */
#pragma once

#include "jce_gfx_types.h"
#include "resource/pak_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load a vertex+fragment shader pair from the PAK and create a program.
 *
 * name: base name without vs_/fs_ prefix (e.g. "color").
 * The function selects the correct backend suffix (dx11/spv/glsl/essl)
 * based on the active renderer.
 *
 * Returns a valid program handle on success, or JCE_INVALID_SHADER on failure. */
JceShaderHandle shader_load_program(const PakArchive *pak, const char *name);

#ifdef __cplusplus
}
#endif
