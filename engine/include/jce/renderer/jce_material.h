/*
 * jce_material.h  Material system binding textures + shader program.
 *
 * LEGACY, and narrower than the name suggests.  JceMaterial holds a type, one
 * diffuse texture and a tint; it groups no shader program and no uniforms, and
 * jce_material_bind() sets only the texture.  The `tint` field is written by
 * jce_material_default() and read by nothing -- the programs this path targets
 * have no tint uniform.
 *
 * The live material system is <jce/renderer/jce_pbr_material.h>.  This header
 * stays public because its TYPES are load-bearing: the scene renderer's
 * internals and the editor's scene-render internals both include it for
 * JceMaterial / JceMaterialType.  Its two functions are unused by anything in
 * this repository (verified 2026-08-31, including the caged_kingdom project).
 */

#ifndef JCE_MATERIAL_H
#define JCE_MATERIAL_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;

/* Pre-defined material types. */
typedef enum {
    JCE_MAT_UNLIT_COLOR,     /* flat color, no texture (color shader) */
    JCE_MAT_UNLIT_TEXTURED,  /* textured, no lighting (textured shader) */
    JCE_MAT_LIT_TEXTURED,    /* textured + directional lighting (mesh shader) */
    JCE_MAT_PBR              /* PBR metallic-roughness (see jce_pbr_material.h) */
} JceMaterialType;

typedef struct {
    JceMaterialType type;
    JceTexture      diffuse;    /* diffuse/albedo texture (UNLIT_TEXTURED, LIT_TEXTURED) */
    uint32_t        tint;       /* ABGR color tint via jce_rgba() */
} JceMaterial;

/* Create a default material of the given type. */
JCE_API JceMaterial jce_material_default(JceMaterialType type);

/* Bind this material's TEXTURE for the next draw.  Despite the name, it binds
   neither a program nor any uniform: get the program from
   jce_renderer_get_program_mesh() (or its siblings) and pass it to jce_submit
   yourself.  Returns void -- the sentence that used to stand here promised a
   returned program handle, which this function has never had.

   JCE_MAT_PBR is a no-op here; PBR materials are bound by
   <jce/renderer/jce_pbr_material.h>, which owns that uniform block. */
JCE_API void jce_material_bind(const JceMaterial *mat, const JceRenderer *r, uint16_t view_id);

JCE_EXTERN_C_END

#endif /* JCE_MATERIAL_H */
