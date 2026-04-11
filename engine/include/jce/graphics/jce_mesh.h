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

/* Submit mesh as wireframe overlay with LEQUAL depth test.
   Used for selection outlines that render on top of solid geometry.
   Caller must set transforms, uniforms, and textures before calling. */
void jce_mesh_submit_wireframe_overlay(const JceMesh *mesh, const JceRenderer *r,
                                       uint16_t view_id);

/* Get vertex/index counts. */
uint32_t jce_mesh_vertex_count(const JceMesh *mesh);
uint32_t jce_mesh_index_count(const JceMesh *mesh);

/* Built-in procedural meshes. */
JceMesh *jce_mesh_create_cube(float size);
JceMesh *jce_mesh_create_plane(float width, float depth, uint32_t subdivs);
JceMesh *jce_mesh_create_plane_ex(float width, float depth,
                                   uint32_t subdivs, float uv_scale);
JceMesh *jce_mesh_create_sphere(float radius);
JceMesh *jce_mesh_create_capsule(float radius, float height);
JceMesh *jce_mesh_create_cylinder(float radius, float height);

/* Submit mesh for PBR rendering using the PBR shader program.
   Caller must call jce_pbr_material_bind() and set transforms before this. */
void jce_mesh_submit_pbr(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

/* Submit mesh for shadow depth pass using the shadow shader program.
   Caller must set transforms and shadow view/proj before calling. */
void jce_mesh_submit_shadow(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MESH_H */
