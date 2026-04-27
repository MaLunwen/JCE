/* jce_shaders.h
 *
 * Runtime shader loading from a PAK archive.
 *
 * Usage:
 *   JcePakArchive *pak = jce_pak_open(assets_pak_data, assets_pak_data_size);
 *   JceShaderHandle prog = shader_load_program(pak, "color");
 *   // loads shaders/vs_color_<backend>.bin + shaders/fs_color_<backend>.bin
 */
#ifndef JCE_SHADERS_H
#define JCE_SHADERS_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_gfx_types.h>

typedef struct JcePakArchive JcePakArchive;

JCE_EXTERN_C_BEGIN

/* Load a vertex+fragment shader pair from the PAK and
 * create a program.
 *
 * name: base name without vs_/fs_ prefix (e.g. "color").
 * Selects the correct backend suffix (dx11/spv/glsl/essl)
 * based on the active renderer.
 *
 * Returns valid handle or JCE_INVALID_SHADER on failure. */
JceShaderHandle shader_load_program(
    const JcePakArchive *pak, const char *name);

/* Load a program with separate VS and FS base names.
 * E.g. shader_load_program_named(pak, "postfx", "tonemap")
 * loads vs_postfx_<backend>.bin + fs_tonemap_<backend>.bin. */
JceShaderHandle shader_load_program_named(
    const JcePakArchive *pak, const char *vs_base, const char *fs_base);

/* Pre-loaded shader set (color + textured + mesh + PBR). */
typedef struct JceShaderSet {
    JceShaderHandle color;
    JceShaderHandle textured;
    JceShaderHandle mesh;
    JceShaderHandle pbr;              /* static PBR */
    JceShaderHandle pbr_skinned;      /* skinned PBR */
    JceShaderHandle shadow;           /* shadow depth */
    JceShaderHandle shadow_skinned;   /* skinned shadow */
    JceShaderHandle terrain;          /* PBR-style terrain (4-layer splat) */
} JceShaderSet;

/* Load all standard shader programs from PAK.
 * Must be called after bgfx is initialized. */
JCE_API JceShaderSet jce_shaders_load_all(const JcePakArchive *pak);

JCE_EXTERN_C_END

#endif /* JCE_SHADERS_H */
