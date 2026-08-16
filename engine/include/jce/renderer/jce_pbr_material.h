/*
 * jce_pbr_material.h  PBR (Physically Based Rendering) material system.
 *
 * Metallic-Roughness workflow following glTF 2.0 specification.
 * Supports albedo, metallic-roughness, normal, AO, and emissive maps.
 *
 * Layer: Graphics (Layer 3) — public API.
 */

#ifndef JCE_PBR_MATERIAL_PUBLIC_H
#define JCE_PBR_MATERIAL_PUBLIC_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceFileSystem JceFileSystem;

typedef struct JceRenderer JceRenderer;

/* ================================================================== */
/* Alpha mode                                                          */
/* ================================================================== */

typedef enum {
    JCE_ALPHA_OPAQUE = 0,   /* Fully opaque, alpha ignored              */
    JCE_ALPHA_MASK,          /* Alpha test with cutoff threshold          */
    JCE_ALPHA_BLEND          /* Standard alpha blending                   */
} JceAlphaMode;

/* ================================================================== */
/* PBR Material                                                        */
/* ================================================================== */

typedef struct JcePbrMaterial {
    /* Texture maps (JCE_TEXTURE_INVALID = not present, use factor).
     *   stage 0: s_albedo
     *   stage 1: s_metalRough  (G = roughness, B = metallic)
     *   stage 2: s_normalMap
     *   stage 3: s_aoMap       (R channel)
     *   stage 4: s_emissive
     */
    JceTexture   albedo_map;
    JceTexture   metallic_roughness_map;
    JceTexture   normal_map;
    JceTexture   ao_map;
    JceTexture   emissive_map;

    /* Factors (multiplied with texture values, or used directly when
     * the corresponding texture is not present). */
    float        base_color_factor[4];   /* RGBA, linear color space     */
    float        metallic_factor;        /* 0.0 .. 1.0                   */
    float        roughness_factor;       /* 0.0 .. 1.0                   */
    float        emissive_factor[3];     /* RGB                          */
    float        normal_scale;           /* normal map strength, default 1.0 */
    float        ao_strength;            /* AO intensity, 0.0 .. 1.0     */

    /* Alpha handling. */
    JceAlphaMode alpha_mode;
    float        alpha_cutoff;           /* MASK mode threshold, default 0.5 */

    /* Render state. */
    bool         double_sided;
    /* Per-renderer "Receive Shadows" off (Unity-style): when true the shader
       skips ALL shadow sampling (CSM + local atlas) for this draw. */
    bool         receive_shadows_off;

    /* Optional custom shader program.  The handle itself is runtime-only
     * (never serialized), but jce_pbr_material_load_json will populate it
     * automatically when the .mat.json declares "customProgramVs" /
     * "customProgramFs" (compiled bgfx .bin blobs produced by the editor's
     * Shader Graph "Compile & Bind").  UINT16_MAX = unset (caller picks the
     * default PBR program; see jce_pbr_material_effective_program). */
    uint16_t     custom_program;
} JcePbrMaterial;

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/* Create a default PBR material (white, metallic=0, roughness=1, opaque). */
JCE_API JcePbrMaterial jce_pbr_material_default(void);

/* Bind the PBR material state (textures, uniforms) for the next draw call.
 * Selects the appropriate PBR shader program.
 * Must be called after jce_renderer_begin_frame_3d. */
JCE_API void jce_pbr_material_bind(const JcePbrMaterial *mat,
                                    const JceRenderer *r, uint16_t view_id);

/* Bind one PBR draw while replacing selected texture slots exactly once.
 * A JCE_TEXTURE_INVALID override keeps the material texture/fallback.
 * Terrain and texture-array batching use this because their shaders reuse
 * s_albedo/s_emissive with draw-specific resources. */
JCE_API void jce_pbr_material_bind_texture_overrides(
    const JcePbrMaterial *mat, const JceRenderer *r, uint16_t view_id,
    JceTexture albedo_override, JceTexture emissive_override);

/* Build the bgfx render state (write masks, depth test, cull, blend) for
 * this material.  Honours alpha_mode (BLEND → src-alpha / inv-src-alpha
 * blend with depth-write disabled) and double_sided (no back-face cull).
 * Returns a value suitable for bgfx_set_state(); 0 is never returned for a
 * valid material.  Used by the renderer so both the inline and render-queue
 * draw paths apply identical transparency / culling state. */
JCE_API uint64_t jce_pbr_material_render_state(const JcePbrMaterial *mat);

/* True when this material renders in the transparent (alpha-blended) pass
 * and therefore needs back-to-front sorting.  MASK/OPAQUE return false. */
JCE_API bool jce_pbr_material_is_transparent(const JcePbrMaterial *mat);

/* Resolve the effective shader program for this material.
 *   - returns mat->custom_program (wrapped) when set (!= UINT16_MAX)
 *   - otherwise returns `default_program` (typically program_pbr).
 * Callers that submit draws should use this to honour graph-generated
 * shaders bound at runtime by the editor's Material Graph "Compile &
 * Bind".  Returns UINT16_MAX inside the wrapper if neither source is
 * valid, in which case the caller should skip submission. */
JCE_API uint16_t jce_pbr_material_effective_program(const JcePbrMaterial *mat,
                                                     uint16_t default_program);

/* Set the global PBR view mode (0=shaded, 1=wireframe, 2=textured/unlit,
 * 3=wireframe+textured). Affects all subsequent jce_pbr_material_bind
 * calls. The scene renderer pushes this once per frame from its config. */
JCE_API void jce_pbr_material_set_view_mode(int mode);

/* Bind ONLY the view-mode component of u_normalScale.
 *
 * For shading paths that do not go through jce_pbr_material_bind at all --
 * grass and foliage each own their whole uniform set -- and therefore never
 * receive the view mode. The symptom is silent and total: those shaders keep
 * rendering their normal lit output in every debug view, so the view shows a
 * partial picture that looks complete. In hidden_cove that was 46% of the
 * viewport, all of it vegetation, which is most of what a forest scene IS.
 *
 * Call once before submitting such a batch. Only .z is meaningful to those
 * shaders; the other components are zeroed, which is correct for them because
 * they read no other field. */
JCE_API void jce_pbr_material_bind_view_mode(void);

/* Release the process-wide cache of graph-generated custom programs created
 * lazily by jce_pbr_material_load_json.  Call once during renderer teardown
 * (after the last frame, before bgfx_shutdown).  Safe to call when empty. */
JCE_API void jce_pbr_material_shutdown(void);

/* ================================================================== */
/* Material file I/O (.mat.json)                                       */
/* ================================================================== */

/* The .mat.json keys accepted for texture slot `slot` (0=albedo,
 * 1=metallic-roughness, 2=normal, 3=AO, 4=emissive — the same order as the
 * out_tex_paths / tex_paths arrays below), in the exact priority order
 * jce_pbr_material_load_json applies them.  Index 0 is the canonical key
 * jce_pbr_material_save_json writes; the rest are accepted aliases.  The
 * returned array is static, immutable and NULL-terminated; NULL is returned
 * for an out-of-range slot.
 *
 * This is the SINGLE authority for material texture keys.  Any other code
 * that has to find a texture reference inside a .mat.json — the editor's
 * material preview resolver, the bundle packer's dependency scan — must
 * iterate this instead of hard-coding its own alias list, or previews and
 * cooked bundles end up disagreeing with the runtime about which texture a
 * material actually uses. */
JCE_API const char *const *jce_pbr_material_texture_keys(int slot);

/* Load PBR material parameters from a .mat.json file.
 * Texture paths are returned via out_tex_paths (not bound to handles); each
 * slot accepts the key set published by jce_pbr_material_texture_keys(), read
 * from a "properties" object when present and from the root otherwise (and
 * as a fallback when a "properties" object exists but omits the key).
 *
 * When the file declares "customProgramVs" + "customProgramFs" (compiled
 * bgfx .bin blobs), both are loaded and linked into out->custom_program so
 * graph-generated shaders persisted by the editor render automatically.
 * That link is why this call is NOT thread-safe — it creates a bgfx program
 * and records it in a process-wide cache with no lock, so only the thread
 * that owns the renderer may call it.  Code on a worker thread that just
 * needs a texture reference must parse the file itself, driving the key set
 * from jce_pbr_material_texture_keys() so it stays in step with this loader.
 * Returns true on success. */
JCE_API bool jce_pbr_material_load_json(const char *path, JcePbrMaterial *out,
                                        char out_tex_paths[5][256]);

/* VFS equivalent of jce_pbr_material_load_json().  Paths remain virtual and
 * are resolved relative to the material path before falling back to the VFS
 * root, so PAK-only scenes retain the same material contract as loose files. */
JCE_API bool jce_pbr_material_load_json_vfs(const JceFileSystem *fs,
                                            const char *path,
                                            JcePbrMaterial *out,
                                            char out_tex_paths[5][256]);

/* Save PBR material parameters to a .mat.json file.
 * tex_paths[0..4] = albedo, metallic_roughness, normal, ao, emissive. */
JCE_API bool jce_pbr_material_save_json(const char *path,
                                 const JcePbrMaterial *mat,
                                 const char tex_paths[5][256]);

/* Attach (or clear) a Shader Graph custom-shader reference on an existing
 * .mat.json, preserving all other fields (read-modify-write).  Used by the
 * editor's Material Graph "Compile & Bind" to persist graph-generated
 * shaders so jce_pbr_material_load_json can re-create the program later.
 *
 *   graph_path  source .matgraph.json (reference only; may be NULL).
 *   vs_bin_path compiled vertex .bin path.
 *   fs_bin_path compiled fragment .bin path.
 *
 * Passing NULL/"" for vs_bin_path AND fs_bin_path removes the keys (detach).
 * Paths are written verbatim; callers should store project-relative paths.
 * Returns true on success. */
JCE_API bool jce_pbr_material_set_graph_shader(const char *mat_path,
                                               const char *graph_path,
                                               const char *vs_bin_path,
                                               const char *fs_bin_path);

JCE_EXTERN_C_END

#endif /* JCE_PBR_MATERIAL_PUBLIC_H */
