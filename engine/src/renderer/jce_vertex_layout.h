/*
 * jce_vertex_layout.h  —  the vertex layout every PBR mesh shares.
 *
 * Position, normal, one UV set, tangent.  Both the static mesh path
 * (jce_mesh.c) and the skinned path (jce_skinned_mesh.c) start from exactly
 * this; the skinned one then appends bone indices and weights.
 *
 * ONE DEFINITION, because they were two identical ones the moment TANGENT was
 * added to the static layout -- and the dedup audit said so within the same
 * change.  A vertex layout that disagrees with itself between two files is not
 * a style problem: the shader reads attributes by SEMANTIC, so a layout
 * missing one leaves that attribute unbound, and what a backend leaves in an
 * unbound attribute is undefined.  That is precisely how the tangent came to
 * be absent here for as long as it was.
 *
 * begin() and end() stay with the CALLER, so the skinned layout can append
 * between them.  A helper that closed the layout would have forced the
 * skinned path to keep its own copy, which is the situation this removes.
 */
#ifndef JCE_VERTEX_LAYOUT_H
#define JCE_VERTEX_LAYOUT_H

#include <bgfx/c99/bgfx.h>

static inline void jce_vertex_layout_add_pbr_base(bgfx_vertex_layout_t *layout)
{
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_POSITION,  3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_NORMAL,    3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    /* xyz = tangent, w = bitangent sign.  glTF's layout, and what vs_pbr.sc
     * multiplies its cross product by. */
    bgfx_vertex_layout_add(layout, BGFX_ATTRIB_TANGENT,   4,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
}

#endif /* JCE_VERTEX_LAYOUT_H */
