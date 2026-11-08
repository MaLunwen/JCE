/* Private immutable creation ports; copied into the owned mesh for teardown. */
#ifndef JCE_MESH_CREATE_INTERNAL_H
#define JCE_MESH_CREATE_INTERNAL_H
#include <jce/renderer/jce_mesh.h>
#include <bgfx/c99/bgfx.h>
typedef struct JceMeshCreateProvider {
    bgfx_renderer_type_t renderer_type;
    const bgfx_memory_t *(*copy)(const void *, uint32_t, void *);
    bgfx_vertex_buffer_handle_t (*create_vertex)(const bgfx_memory_t *, const bgfx_vertex_layout_t *, void *);
    bgfx_index_buffer_handle_t (*create_index)(const bgfx_memory_t *, void *);
    void (*destroy_vertex)(bgfx_vertex_buffer_handle_t, void *);
    void (*destroy_index)(bgfx_index_buffer_handle_t, void *);
    void *user; /* Must outlive the mesh; production uses NULL. */
} JceMeshCreateProvider;
JceMesh *jce_mesh_create_with_provider(const JceMeshVertex *, uint32_t,
    const uint32_t *, uint32_t, const JceMeshCreateProvider *);
/* Same source vertex/index data used by the GPU cube uploader. */
void jce_mesh_cube_geometry(float size, JceMeshVertex vertices[24],
                            uint32_t indices[36]);
#endif
