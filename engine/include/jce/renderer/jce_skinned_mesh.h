/*
 * jce_skinned_mesh.h  Skinned GPU mesh with bone weights.
 *
 * Extends JceMesh with tangent, joint indices, and bone weights for
 * skeletal animation and PBR normal mapping.
 *
 * Also provides JcePbrVertex for static meshes that need tangents
 * (normal mapping) but no skinning.
 *
 * Layer: Renderer (Layer 1).
 */

#ifndef JCE_SKINNED_MESH_H
#define JCE_SKINNED_MESH_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSkinnedMesh JceSkinnedMesh;
typedef struct JceRenderer    JceRenderer;

/* ================================================================== */
/* Constants                                                           */
/* ================================================================== */

#define JCE_MAX_BONE_INFLUENCES 4

/* GPU bone-palette ceiling. The skinned vertex shaders address bones through
 * the bgfx u_model[] predefined-uniform array, whose size is fixed at compile
 * time by BGFX_CONFIG_MAX_BONES (see tools/compile_shaders.cmake). This define
 * MUST stay equal to that value: vs_pbr_skinned.sc / vs_shadow_skinned.sc clamp
 * the joint index to JCE_MAX_BONES-1, and a skeleton exceeding it is rejected
 * with a load-time LOG_ERROR in jce_skeleton_create() so artists see the cause
 * instead of silently-collapsed bones.
 *
 * 128 is the practical ceiling for the uniform-array path: 128 mat4 = 512 vec4,
 * which fits every real desktop GL / GLES3 / D3D11 / Metal / SPIR-V vertex
 * uniform budget the engine targets. Going higher (256) would require a bone-
 * matrix TEXTURE sampled via texelFetch, which the desktop glsl=120 profile
 * cannot compile (texelFetch needs GLSL 1.30+) and which has no free sampler
 * stage in the 16-stage fs_pbr fragment program. */
#define JCE_MAX_BONES           128

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
JCE_API void jce_skinned_mesh_destroy(JceSkinnedMesh *mesh);

/* Submit the mesh for rendering.
 * For skinned meshes, call jce_skinned_mesh_set_bones() first.
 * Caller must bind material/program before this. */
JCE_API void JCE_CALL jce_skinned_mesh_submit(const JceSkinnedMesh *mesh,
                                              const JceRenderer *r, uint16_t view_id);

/* Submit a wireframe overlay (line topology, LEQUAL depth) of the
 * mesh's geometry. For skinned meshes the bone palette must already
 * have been uploaded via jce_skinned_mesh_set_bones() (or
 * bgfx_set_transform for non-skinned PBR variants). */
JCE_API void JCE_CALL jce_skinned_mesh_submit_wireframe_overlay(
    const JceSkinnedMesh *mesh, const JceRenderer *r, uint16_t view_id);

/* Submit mesh to an object-ID picking pass.  For skinned meshes the bone
 * palette must already be uploaded with jce_skinned_mesh_set_bones(). */
JCE_API void JCE_CALL jce_skinned_mesh_submit_pick_id(
    const JceSkinnedMesh *mesh,
    const JceRenderer *r,
    uint16_t view_id,
    JceShaderHandle program,
    bool double_sided);

/* Upload bone matrices for the next skinned draw call.
 * joint_matrices: array of [num_joints] mat4, each = globalTransform * inverseBindMatrix.
 * Uses bgfx_set_transform to populate u_model[0..N]. */
JCE_API void JCE_CALL jce_skinned_mesh_set_bones(const jce_mat4 *joint_matrices,
                                                 uint32_t num_joints);

/* Submit the mesh to a shadow/depth pass (depth-only state, front-face
 * cull).  For skinned geometry the bone palette must already have been
 * uploaded via jce_skinned_mesh_set_bones() and `program` must be the
 * skinned shadow program; for static-PBR geometry set a single transform
 * and pass the static shadow program. No-op if program is invalid. */
JCE_API void JCE_CALL jce_skinned_mesh_submit_shadow(const JceSkinnedMesh *mesh,
                                                     const JceRenderer *r,
                                                     uint16_t view_id,
                                                     JceShaderHandle program);

/* Query whether this mesh has skinning data. */
JCE_API bool     jce_skinned_mesh_is_skinned(const JceSkinnedMesh *mesh);
JCE_API uint32_t jce_skinned_mesh_vertex_count(const JceSkinnedMesh *mesh);
JCE_API uint32_t jce_skinned_mesh_index_count(const JceSkinnedMesh *mesh);

JCE_EXTERN_C_END

#endif /* JCE_SKINNED_MESH_H */
