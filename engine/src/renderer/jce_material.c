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

    case JCE_MAT_PBR:
        /* Bound by jce_pbr_material.h, which owns the metallic-roughness
         * uniform block and its texture set; this legacy path has neither.
         * Explicit rather than absent: with no case and no default, a PBR
         * material passed here silently did nothing, which is the one
         * outcome a caller cannot distinguish from success. */
        break;
    }

    /* NOTE: mat->tint is NOT applied here.  jce_material_default() sets it to
     * opaque white and nothing in this function reads it -- the three shader
     * programs this path targets have no tint uniform.  Said out loud because
     * the struct field reads like it is honoured. */
}
