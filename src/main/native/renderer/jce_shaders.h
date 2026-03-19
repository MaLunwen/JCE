/* jce_shaders.h
 *
 * Runtime bgfx shader loading from a PAK archive.
 *
 * Usage:
 *   PakArchive *pak = pak_open(assets_pak_data, assets_pak_data_size);
 *   bgfx_program_handle_t prog = shader_load_program(pak, "color");
 *   // loads shaders/vs_color_<backend>.bin + shaders/fs_color_<backend>.bin
 */
#pragma once

#include <bgfx/c99/bgfx.h>
#include "resource/pak_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load a vertex+fragment shader pair from the PAK and create a bgfx program.
 *
 * name: base name without vs_/fs_ prefix (e.g. "color").
 * The function selects the correct backend suffix (dx11/spv/glsl/essl)
 * based on the active bgfx renderer.
 *
 * Returns a valid program handle on success, or { UINT16_MAX } on failure. */
bgfx_program_handle_t shader_load_program(PakArchive *pak, const char *name);

#ifdef __cplusplus
}
#endif
