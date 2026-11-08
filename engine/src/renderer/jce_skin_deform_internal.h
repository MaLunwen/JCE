/* Private linear blend skinning without a graphics device. */
#ifndef JCE_SKIN_DEFORM_INTERNAL_H
#define JCE_SKIN_DEFORM_INTERNAL_H

#include <jce/renderer/jce_skinned_mesh.h>

/* Deform base or pre-morphed vertices in place using the model-space palette. */
void jce_skin_deform_vertices(JceSkinnedVertex *vertices, uint32_t count,
                              const jce_mat4 *palette, uint32_t joints);

#endif
