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

/* Filesystem overlay variant — reads .bin from <dev_dir>/shaders/...
 * via SDL_IO instead of from PAK.  Used by the shader manager when
 * a dev directory is registered (hot-reload support).  Returns
 * JCE_INVALID_SHADER on missing file or backend mismatch. */
JceShaderHandle shader_load_program_fs(
    const char *dev_dir, const char *name);
JceShaderHandle shader_load_program_fs_named(
    const char *dev_dir, const char *vs_base, const char *fs_base);

/* Resolve each stage independently: an existing development-overlay binary
 * wins for that stage, otherwise the stage is loaded from the project PAK
 * with the normal embedded-engine fallback.  This is the project-shader path:
 * a project fragment stage can be hot-loaded while the shared engine vertex
 * stage remains embedded. */
JceShaderHandle shader_load_program_overlay_named(
    const char *dev_dir, const JcePakArchive *pak,
    const char *vs_base, const char *fs_base);

/* Destroy a program created by the loaders above.  No-op on an invalid
 * handle.  Non-renderer layers that own program lifetimes (the resource
 * shader cache) must release through this instead of unwrapping the
 * handle into a bgfx one themselves — jce_gfx_types.h reserves that
 * conversion to renderer .c files. */
JCE_API void jce_shader_program_destroy(JceShaderHandle prog);

/* Shader-binary suffix ("dx11"/"spv"/"glsl"/"essl"/"mtl") for the live
 * backend, or NULL when the backend ships no compiled variants.  Single
 * source of truth for anyone that has to name a .bin on disk. */
JCE_API const char *jce_shaders_backend_suffix(void);

/* Pre-loaded shader set (color + textured + mesh + PBR). */
typedef struct JceShaderSet {
    JceShaderHandle color;
    JceShaderHandle textured;
    JceShaderHandle mesh;
    JceShaderHandle pbr;              /* static PBR */
    JceShaderHandle pbr_inst;         /* static PBR — GPU-instanced variant */
    /* Per-instance-tint instanced PBR (large-world-opt P1 #7): vs_pbr_inst_tint
     * + fs_pbr_tint.  The instance buffer carries a 5th vec4 (i_data4 = the
     * per-entity baseColor tint) so baseColor-only copies of one mesh batch into
     * one instanced submit instead of falling back to solo draws.  Optional: a
     * pak built before the variant existed leaves the handle invalid and the
     * renderer simply keeps tinted entities on the solo path. */
    JceShaderHandle pbr_inst_tint;    /* instanced PBR — per-instance tint */
    /* Texture-diverse instanced PBR: vs_pbr_inst_tex_array + fs_pbr_inst_tex_array.
     * The instance buffer carries a 6th vec4 (i_data5.x = albedo 2D-array LAYER),
     * and s_albedo is a SAMPLER2DARRAY, so copies of one mesh that differ by their
     * albedo TEXTURE (not just a baseColor factor) batch into one instanced submit.
     * Optional (essl1/GLES2-excluded, older pak): invalid handle → texture-diverse
     * entities stay on the solo path. */
    JceShaderHandle pbr_inst_tex_array; /* instanced PBR — per-instance albedo array layer */
    JceShaderHandle pbr_inst_fade;      /* instanced PBR — LOD cross-fade dither (千万 ②) */
    JceShaderHandle pbr_skinned;      /* skinned PBR */
    /* Forward+ clustered fragment variant (fs_pbr_fwdplus): same vertex
     * shaders, IES dropped + clustered point/spot loop.  Selected only when
     * the r.forwardplus cvar is on; default off keeps the non-variant pbr*. */
    JceShaderHandle pbr_fwdplus;          /* static PBR — Forward+ fragment */
    JceShaderHandle pbr_inst_fwdplus;     /* instanced PBR — Forward+ fragment */
    JceShaderHandle pbr_skinned_fwdplus;  /* skinned PBR — Forward+ fragment */
    /* Per-character toon (stylized-slice §5.6): vs_pbr_skinned + fs_pbr_toon
     * (cel ramp + rim).  Optional: a pak built before the variant existed
     * leaves the handle invalid → renderer degrades to standard skinned PBR. */
    JceShaderHandle pbr_toon;
    /* Inverted-hull silhouette outline: vs_pbr_skinned_outline + fs_outline
     * (FRONT-cull, flat linear).  Optional; invalid => no outline. */
    JceShaderHandle outline_skinned;
    JceShaderHandle shadow;           /* shadow depth */
    JceShaderHandle shadow_inst;      /* shadow depth — GPU-instanced variant */
    JceShaderHandle shadow_skinned;   /* skinned shadow */
    JceShaderHandle shadow_vsm;       /* variance shadow map (depth, depth^2) */
    JceShaderHandle terrain;          /* PBR-style terrain (4-layer splat) */
} JceShaderSet;

/* Load all standard shader programs from PAK.
 * Must be called after bgfx is initialized. */
JCE_API JceShaderSet jce_shaders_load_all(const JcePakArchive *pak);

/* Same as jce_shaders_load_all but tries `<dev_dir>/shaders/*.bin` on
 * the filesystem first for each shader, falling back to PAK on a
 * per-shader basis.  When dev_dir is NULL or empty, behaves identically
 * to jce_shaders_load_all.  Used by the editor's "Reload Shaders"
 * action to demonstrate end-to-end hot-reload from disk. */
JCE_API JceShaderSet jce_shaders_load_all_fs(const char *dev_dir,
                                             const JcePakArchive *pak);

/* The engine-embedded shader PAK (baked into jce_renderer when
 * JCE_EMBED_ENGINE_SHADERS is ON), or NULL when no embedded pak is present.
 *
 * Engine subsystems that load their own shaders directly from a
 * caller-supplied scene/game PAK (e.g. decals, GPU particles) MUST fall back
 * to this when the scene PAK lacks the shader — otherwise they break on every
 * build that bakes the engine shaders out of the scene PAK (the editor ships
 * editor_assets.pak with zero shaders).  Mirrors the fallback in the standard
 * shader loader (load_single).  Cached; safe to call after bgfx init. */
JCE_API const JcePakArchive *jce_shaders_embedded_engine_pak(void);

JCE_EXTERN_C_END

#endif /* JCE_SHADERS_H */
