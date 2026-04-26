/*
 * jce_material.h  Material system binding textures + shader program.
 *
 * A material groups a shader program with its texture slots and uniforms.
 * Pre-defined material types cover common use cases.
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
JceMaterial jce_material_default(JceMaterialType type);

/* Bind the material state (program, textures, uniforms) for the next draw.
   Returns the program handle to use with bgfx_submit. */
void jce_material_bind(const JceMaterial *mat, const JceRenderer *r, uint16_t view_id);

JCE_EXTERN_C_END

#endif /* JCE_MATERIAL_H */
