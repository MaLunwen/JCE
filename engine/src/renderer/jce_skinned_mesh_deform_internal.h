/* Private CPU deformation and posed geometry submission. */
#ifndef JCE_SKINNED_MESH_DEFORM_INTERNAL_H
#define JCE_SKINNED_MESH_DEFORM_INTERNAL_H

#include "jce_skin_deform_internal.h"
#include <bgfx/c99/bgfx.h>

/* Use the same live API/build profile that admits stock GPU skinning. */
bool jce_skinned_mesh_cpu_skin_required(void);

/* Direct draw fallback. The transient allocation lives for the current frame. */
bool jce_skinned_mesh_deform_transient(const JceSkinnedMesh *mesh,
                                     const jce_mat4 *palette, uint32_t joints,
                                     bgfx_transient_vertex_buffer_t *out);

typedef struct JceSkinVertexSource {
    const bgfx_transient_vertex_buffer_t *transient;
    uint16_t dynamic_idx;
} JceSkinVertexSource;

/* A null source or an invalid dynamic handle selects the mesh's original VB.
 * A transient source takes precedence over a dynamic handle. The colour call
 * binds geometry and state. Shadow, wireframe and pick also submit the program. */
void jce_skinned_mesh_submit_vertices(const JceSkinnedMesh *mesh,
    const JceRenderer *r, uint16_t view, const JceSkinVertexSource *source);
void jce_skinned_mesh_submit_shadow_vertices(const JceSkinnedMesh *mesh,
    const JceRenderer *r, uint16_t view, JceShaderHandle program,
    const JceSkinVertexSource *source);
void jce_skinned_mesh_submit_wireframe_vertices(const JceSkinnedMesh *mesh,
    const JceRenderer *r, uint16_t view, JceShaderHandle program,
    const JceSkinVertexSource *source, bool overlay);
void jce_skinned_mesh_submit_pick_vertices(const JceSkinnedMesh *mesh,
    const JceRenderer *r, uint16_t view, JceShaderHandle program,
    bool double_sided, const JceSkinVertexSource *source);

#endif
