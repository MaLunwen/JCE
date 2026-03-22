/*
 * jce_mesh.h  GPU mesh (vertex + index buffers).
 *
 * Represents a renderable mesh with position, normal, and texcoord attributes.
 * Created from raw vertex/index data or loaded from model files via assimp.
 */

#ifndef JCE_MESH_H
#define JCE_MESH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceMesh    JceMesh;
typedef struct JceRenderer JceRenderer;
typedef struct PakArchive  PakArchive;

/* Mesh vertex: position + normal + texcoord. */
typedef struct {
    float pos[3];
    float normal[3];
    float uv[2];
} JceMeshVertex;

/* Create a mesh from raw vertex/index data.
   Copies the data  caller retains ownership of arrays. */
JceMesh *jce_mesh_create(const JceMeshVertex *vertices, uint32_t num_verts,
                          const uint32_t *indices, uint32_t num_indices);

/* Load a mesh from a model file in PAK (e.g. "models/chalet.obj").
   Uses assimp. Returns first mesh in the file. */
JceMesh *jce_mesh_load(const PakArchive *pak, const char *asset_path);

/* Destroy a mesh and free GPU buffers. */
void jce_mesh_destroy(JceMesh *mesh);

/* Submit the mesh for rendering on the given view with the given program.
   Caller must set transforms and uniforms before calling this. */
void jce_mesh_submit(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

/* Get vertex/index counts. */
uint32_t jce_mesh_vertex_count(const JceMesh *mesh);
uint32_t jce_mesh_index_count(const JceMesh *mesh);

/* Built-in procedural meshes. */
JceMesh *jce_mesh_create_cube(float size);
JceMesh *jce_mesh_create_plane(float width, float depth, uint32_t subdivs);
JceMesh *jce_mesh_create_plane_ex(float width, float depth,
                                   uint32_t subdivs, float uv_scale);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MESH_H */
