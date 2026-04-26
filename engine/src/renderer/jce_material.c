/*
 * jce_material.c  Material system implementation.
 */

#include <jce/renderer/jce_material.h>

#include "jce_renderer_internal.h"

#include <bgfx/c99/bgfx.h>

JceMaterial jce_material_default(JceMaterialType type)
{
    JceMaterial mat;
    mat.type    = type;
    mat.diffuse = JCE_TEXTURE_INVALID;
    mat.tint    = 0xFFFFFFFF; /* white, fully opaque (ABGR) */
    return mat;
}

void jce_material_bind(const JceMaterial *mat, const JceRenderer *r, uint16_t view_id)
{
    if (!mat || !r) return;
    (void)view_id;

    switch (mat->type) {
    case JCE_MAT_UNLIT_COLOR:
        /* No textures to bind. Caller uses color program. */
        break;

    case JCE_MAT_UNLIT_TEXTURED:
        if (jce_texture_valid(mat->diffuse)) {
            bgfx_texture_handle_t th = { mat->diffuse.idx };
            JceUniformHandle uh = jce_renderer_get_tex_uniform(r);
            bgfx_uniform_handle_t su = { uh.idx };
            bgfx_set_texture(0, su, th, UINT32_MAX);
        }
        break;

    case JCE_MAT_LIT_TEXTURED:
        if (jce_texture_valid(mat->diffuse)) {
            bgfx_texture_handle_t th = { mat->diffuse.idx };
            JceUniformHandle uh2 = jce_renderer_get_tex_uniform(r);
            bgfx_uniform_handle_t su2 = { uh2.idx };
            bgfx_set_texture(0, su2, th, UINT32_MAX);
        }
        /* Light uniforms are set by the lighting system, not the material. */
        break;
    }
}
