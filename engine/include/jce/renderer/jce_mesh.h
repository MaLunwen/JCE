/*
 * jce_mesh.h  GPU mesh (vertex + index buffers).
 *
 * Represents a renderable mesh with position, normal, and texcoord attributes.
 * Created from raw vertex/index data or loaded from model files via assimp.
 */

#ifndef JCE_MESH_H
#define JCE_MESH_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceMesh    JceMesh;
typedef struct JceRenderer JceRenderer;
typedef struct JcePakArchive  JcePakArchive;

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
JCE_API JceMesh *jce_mesh_load(const JcePakArchive *pak, const char *asset_path);

/* Destroy a mesh and free GPU buffers. */
JCE_API void jce_mesh_destroy(JceMesh *mesh);

/* Submit the mesh for rendering on the given view with the given program.
   Caller must set transforms and uniforms before calling this. */
JCE_API void jce_mesh_submit(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

/* Submit mesh as wireframe overlay with LEQUAL depth test.
   Used for selection outlines that render on top of solid geometry.
   Caller must set transforms, uniforms, and textures before calling. */
void jce_mesh_submit_wireframe_overlay(const JceMesh *mesh, const JceRenderer *r,
                                       uint16_t view_id);

/* Get vertex/index counts. */
JCE_API uint32_t jce_mesh_vertex_count(const JceMesh *mesh);
JCE_API uint32_t jce_mesh_index_count(const JceMesh *mesh);

/* Raw bgfx handle indices for direct queue submission (UINT16_MAX if invalid). */
JCE_API uint32_t jce_mesh_get_vbh(const JceMesh *mesh);
JCE_API uint32_t jce_mesh_get_ibh(const JceMesh *mesh);

/* Local-space axis-aligned bounding box (computed at create time from
 * the supplied vertices). For an empty / null mesh, both arrays are
 * filled with zeros. Used by frustum culling. */
JCE_API void jce_mesh_get_aabb(const JceMesh *mesh,
                                float out_min[3], float out_max[3]);

/* Built-in procedural meshes. */
JCE_API JceMesh *jce_mesh_create_cube(float size);
JCE_API JceMesh *jce_mesh_create_plane(float width, float depth, uint32_t subdivs);
JceMesh *jce_mesh_create_plane_ex(float width, float depth,
                                   uint32_t subdivs, float uv_scale);
JCE_API JceMesh *jce_mesh_create_sphere(float radius);
JCE_API JceMesh *jce_mesh_create_capsule(float radius, float height);
JCE_API JceMesh *jce_mesh_create_cylinder(float radius, float height);

/* Submit mesh for PBR rendering using the PBR shader program.
   Caller must call jce_pbr_material_bind() and set transforms before this. */
JCE_API void jce_mesh_submit_pbr(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

/* Variant of jce_mesh_submit_pbr that uses an explicit shader program
   handle instead of the renderer's default PBR program.  Used by the
   editor material-graph preview to render a sphere with a custom
   graph-generated program (or any other valid program with PBR vertex
   layout).  When `program.idx == UINT16_MAX`, falls back to the
   renderer's default PBR program (same behaviour as jce_mesh_submit_pbr). */
JCE_API void jce_mesh_submit_pbr_with_program(const JceMesh        *mesh,
                                               const JceRenderer    *r,
                                               uint16_t              view_id,
                                               JceShaderHandle       program);

/* Submit mesh for terrain rendering using the terrain shader program.
   Caller must bind splat / layer textures and u_terrainParams before this
   (in addition to the usual PBR uniforms / lighting / shadows). */
JCE_API void jce_mesh_submit_terrain(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

/* Submit mesh for shadow depth pass using the shadow shader program.
   Caller must set transforms and shadow view/proj before calling. */
JCE_API void jce_mesh_submit_shadow(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

/* Submit mesh using the mesh program WITHOUT overriding bgfx_set_state.
   Caller must set state, transforms, uniforms, and textures before this. */
JCE_API void jce_mesh_submit_overlay(const JceMesh *mesh, const JceRenderer *r, uint16_t view_id);

JCE_EXTERN_C_END

#endif /* JCE_MESH_H */
