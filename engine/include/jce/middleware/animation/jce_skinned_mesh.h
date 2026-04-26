/*
 * jce_skinned_mesh.h  Skinned GPU mesh with bone weights.
 *
 * Extends JceMesh with tangent, joint indices, and bone weights for
 * skeletal animation and PBR normal mapping.
 *
 * Also provides JcePbrVertex for static meshes that need tangents
 * (normal mapping) but no skinning.
 *
 * Layer: Animation System (Layer 3).
 */

#ifndef JCE_SKINNED_MESH_H
#define JCE_SKINNED_MESH_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSkinnedMesh JceSkinnedMesh;
typedef struct JceRenderer    JceRenderer;

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define JCE_MAX_BONE_INFLUENCES 4
#define JCE_MAX_BONES           64   /* bgfx u_model[] default capacity */

/* ================================================================== */
/* Vertex formats                                                      */
/* ================================================================== */

/* Static PBR vertex: position + normal + UV + tangent (for normal mapping). */
typedef struct {
    float pos[3];
    float normal[3];
    float uv[2];
    float tangent[4];   /* xyz = tangent direction, w = handedness (+1 or -1) */
} JcePbrVertex;

/* Skinned vertex: PBR attributes + bone influences. */
typedef struct {
    float   pos[3];
    float   normal[3];
    float   uv[2];
    float   tangent[4];
    uint8_t joints[JCE_MAX_BONE_INFLUENCES];   /* bone indices (0-255) */
    float   weights[JCE_MAX_BONE_INFLUENCES];   /* bone weights (sum to 1.0) */
} JceSkinnedVertex;

/* ================================================================== */
/* Skinned mesh API                                                    */
/* ================================================================== */

/* Create a skinned mesh from raw vertex/index data.
 * Copies the data; caller retains ownership. */
JceSkinnedMesh *jce_skinned_mesh_create(
    const JceSkinnedVertex *vertices, uint32_t num_verts,
    const uint32_t *indices, uint32_t num_indices);

/* Create a static PBR mesh (tangents, no skinning) from raw data. */
JceSkinnedMesh *jce_pbr_mesh_create(
    const JcePbrVertex *vertices, uint32_t num_verts,
    const uint32_t *indices, uint32_t num_indices);

/* Destroy a skinned mesh and free GPU buffers. */
void jce_skinned_mesh_destroy(JceSkinnedMesh *mesh);

/* Submit the mesh for rendering.
 * For skinned meshes, call jce_skinned_mesh_set_bones() first.
 * Caller must bind material/program before this. */
void jce_skinned_mesh_submit(const JceSkinnedMesh *mesh,
                              const JceRenderer *r, uint16_t view_id);

/* Upload bone matrices for the next skinned draw call.
 * joint_matrices: array of [num_joints] mat4, each = globalTransform * inverseBindMatrix.
 * Uses bgfx_set_transform to populate u_model[0..N]. */
void jce_skinned_mesh_set_bones(const jce_mat4 *joint_matrices,
                                 uint32_t num_joints);

/* Query whether this mesh has skinning data. */
bool     jce_skinned_mesh_is_skinned(const JceSkinnedMesh *mesh);
uint32_t jce_skinned_mesh_vertex_count(const JceSkinnedMesh *mesh);
uint32_t jce_skinned_mesh_index_count(const JceSkinnedMesh *mesh);

JCE_EXTERN_C_END

#endif /* JCE_SKINNED_MESH_H */
