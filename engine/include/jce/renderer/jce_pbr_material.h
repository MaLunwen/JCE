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

JCE_EXTERN_C_BEGIN

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
} JcePbrMaterial;

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/* Create a default PBR material (white, metallic=0, roughness=1, opaque). */
JcePbrMaterial jce_pbr_material_default(void);

/* Bind the PBR material state (textures, uniforms) for the next draw call.
 * Selects the appropriate PBR shader program.
 * Must be called after jce_renderer_begin_frame_3d. */
void jce_pbr_material_bind(const JcePbrMaterial *mat,
                            const JceRenderer *r, uint16_t view_id);

/* Set the global PBR view mode (0=shaded, 1=wireframe, 2=textured/unlit,
 * 3=wireframe+textured). Affects all subsequent jce_pbr_material_bind
 * calls. The scene renderer pushes this once per frame from its config. */
void jce_pbr_material_set_view_mode(int mode);

/* ================================================================== */
/* Material file I/O (.mat.json)                                       */
/* ================================================================== */

/* Load PBR material parameters from a .mat.json file.
 * Texture paths are stored in the struct name fields (not loaded).
 * Returns true on success. */
bool jce_pbr_material_load_json(const char *path, JcePbrMaterial *out,
                                 char out_tex_paths[5][256]);

/* Save PBR material parameters to a .mat.json file.
 * tex_paths[0..4] = albedo, metallic_roughness, normal, ao, emissive. */
bool jce_pbr_material_save_json(const char *path,
                                 const JcePbrMaterial *mat,
                                 const char tex_paths[5][256]);

JCE_EXTERN_C_END

#endif /* JCE_PBR_MATERIAL_PUBLIC_H */
