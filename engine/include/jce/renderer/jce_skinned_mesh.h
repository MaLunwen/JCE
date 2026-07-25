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

/* Max in-asset auto-LOD index buffers a single mesh may carry (large-world-opt
 * P1 #6).  The cook emits up to JCE_MESH_LOD_DEFAULT_LEVEL_COUNT (3) reduced
 * levels; this is the renderer-side ceiling (matches the LODGroup component's
 * per-level cap so a level index is always bindable). */
#define JCE_SM_MAX_LOD 8

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
 * Copies the data; caller retains ownership.
 *
 * retain_cpu: when true the source vertex array is ALSO kept in a CPU-side copy
 * (jce_skinned_mesh_base_verts) so a per-instance morph deform can read the
 * undeformed base and re-upload into a dynamic VB.  Default (false) keeps the
 * legacy behavior byte-identical with zero extra RAM — callers MUST only opt in
 * for morph-bearing primitives (FEATURE 3.1). */
JCE_API JceSkinnedMesh *jce_skinned_mesh_create(
    const JceSkinnedVertex *vertices, uint32_t num_verts,
    const uint32_t *indices, uint32_t num_indices,
    bool retain_cpu);

/* Create a static PBR mesh (tangents, no skinning) from raw data.
 * retain_cpu: see jce_skinned_mesh_create (kept base verts for morph deform). */
JCE_API JceSkinnedMesh *jce_pbr_mesh_create(
    const JcePbrVertex *vertices, uint32_t num_verts,
    const uint32_t *indices, uint32_t num_indices,
    bool retain_cpu);

/* Destroy a skinned mesh and free GPU buffers. */
JCE_API void jce_skinned_mesh_destroy(JceSkinnedMesh *mesh);

/* Submit the mesh for rendering.
 * For skinned meshes, call jce_skinned_mesh_set_bones() first.
 * Caller must bind material/program before this. */
JCE_API void JCE_CALL jce_skinned_mesh_submit(const JceSkinnedMesh *mesh,
                                              const JceRenderer *r, uint16_t view_id);

/* Set the per-submit double-sided override consumed by the NEXT
 * jce_skinned_mesh_submit (and its morph variant): true drops back-face culling
 * so a two-sided material draws both winding orders.  Render-thread only;
 * jce_model_draw* sets it from each primitive's material->double_sided and
 * resets it to false afterwards.  Default false = legacy single-sided (CULL_CW). */
JCE_API void JCE_CALL jce_skinned_mesh_set_submit_double_sided(bool on);

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

/* Morph-deform color submit (FEATURE 3.1): identical to jce_skinned_mesh_submit
 * EXCEPT the vertex buffer comes from a per-instance dynamic VB instead of the
 * mesh's shared static VB.  dyn_vb_idx is a bgfx_dynamic_vertex_buffer_handle_t
 * .idx (passed bare so this header stays bgfx-free); UINT16_MAX makes this a
 * literal fall-through to jce_skinned_mesh_submit.  The index buffer, wireframe
 * branch, and render state are byte-identical to the static submit — only the
 * vertex source changes, so the unchanged skinned program reads morphed verts. */
JCE_API void JCE_CALL jce_skinned_mesh_submit_morphed(const JceSkinnedMesh *mesh,
                                                      const JceRenderer *r,
                                                      uint16_t view_id,
                                                      uint16_t dyn_vb_idx);

/* Morph-deform shadow submit: mirror of jce_skinned_mesh_submit_shadow that
 * binds the per-instance dynamic VB (dyn_vb_idx) instead of the static VB, so
 * the cast silhouette matches the morphed, lit mesh.  UINT16_MAX => identical
 * to jce_skinned_mesh_submit_shadow. */
JCE_API void JCE_CALL jce_skinned_mesh_submit_shadow_morphed(
    const JceSkinnedMesh *mesh, const JceRenderer *r, uint16_t view_id,
    JceShaderHandle program, uint16_t dyn_vb_idx);

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

/* Bind-only (no submit) for an INSTANCED shadow draw: binds the mesh's VB +
 * TRIANGLE index buffer + depth-only state; the caller then sets the
 * per-instance data buffer and submits the instanced shadow program.  Triangle-
 * only (no wireframe branch), matching jce_skinned_mesh_submit_shadow. */
JCE_API void JCE_CALL jce_skinned_mesh_bind_shadow(const JceSkinnedMesh *mesh);

/* Query whether this mesh has skinning data. */
JCE_API bool     jce_skinned_mesh_is_skinned(const JceSkinnedMesh *mesh);
JCE_API uint32_t jce_skinned_mesh_vertex_count(const JceSkinnedMesh *mesh);
JCE_API uint32_t jce_skinned_mesh_index_count(const JceSkinnedMesh *mesh);

/* Raw buffer handle idxs (mirror of jce_mesh_get_vbh/ibh) — lets a BIND-POSE
 * skinned mesh ride the render-queue instancing paths as if static: its
 * vertices are model-space, and the extra bone attributes in the VB are
 * ignored by programs that don't read them.  UINT16_MAX when mesh is NULL. */
JCE_API uint32_t jce_skinned_mesh_get_vbh(const JceSkinnedMesh *mesh);
JCE_API uint32_t jce_skinned_mesh_get_ibh(const JceSkinnedMesh *mesh);

/* ================================================================== */
/* In-asset auto-LOD (large-world-opt P1 #6)                            */
/* ================================================================== */

/* Append one reduced-index LOD level (1-based external naming; stored 0-based).
 * The level shares this mesh's vertex buffer — `indices` must address [0,
 * vertex_count).  Copies the data; caller retains ownership.  Returns the new
 * level count, or the unchanged count on bad input / capacity.  Called by the
 * model loader right after create; not thread-safe with submits. */
JCE_API uint32_t jce_skinned_mesh_add_lod(JceSkinnedMesh *mesh,
                                          const uint32_t *indices,
                                          uint32_t num_indices);

/* Number of in-asset reduced LOD levels (0 = base mesh only). */
JCE_API uint32_t jce_skinned_mesh_lod_count(const JceSkinnedMesh *mesh);

/* Index count of reduced LOD `level` (0-based: level 0 = first reduced level).
 * Returns the base index_count when level is out of range, so a caller can
 * always quote a triangle figure. */
JCE_API uint32_t jce_skinned_mesh_lod_index_count(const JceSkinnedMesh *mesh,
                                                  uint32_t level);

/* Raw INDEX buffer handle idx of reduced LOD `level` (0-based: level 0 = first
 * reduced level).  Shares the base VERTEX buffer (jce_skinned_mesh_get_vbh) —
 * LODs differ only in the index set.  Returns the base ibh idx when `level` is
 * out of range (or no LODs).  Pairs with jce_skinned_mesh_lod_index_count to
 * drive a GPU-indirect per-LOD draw (千万 S4 LOD-in-cull) that binds the LOD
 * index buffer without the per-instance submit path. */
JCE_API uint32_t jce_skinned_mesh_lod_ibh(const JceSkinnedMesh *mesh,
                                          uint32_t level);

/* Nanite-lite V1/V3 meshlet sidecar: install/query the meshlet-grouped
 * alternate index buffer (each cluster's triangles contiguous; shares the
 * base VB) plus per-meshlet GPU cull records (6 vec4 each: {offset bits,
 * count bits, 0, 0}, {sphere cx,cy,cz,r}, {cone axis xyz, cutoff — meshopt
 * convention}, {own_error, parent_error, 0, 0}, {own-group sphere xyzr},
 * {parent-group sphere xyzr}) uploaded as a static COMPUTE_READ buffer for
 * the cluster-cull compute.  desc = 2 u32 per meshlet {index_offset,
 * index_count}; bounds = 8 f32 per meshlet; errors = 10 f32 per meshlet
 * {own_error, parent_error, own-group sphere xyzr, parent-group sphere
 * xyzr} — errors in OBJECT units (V3 cluster-LOD DAG cut; the shared group
 * spheres make a child's parent-test and its parent's own-test evaluate the
 * identical (error, sphere) pair, so the cut is exactly complementary — no
 * holes, no double-draw) or NULL — NULL marks every cluster {0, +BIG} =
 * "leaf with no parent", which the cut always draws (plain V1 behaviour).
 * Returns the installed count (0 = rejected).  Raw bgfx idx getters return
 * UINT16_MAX when no sidecar. */
JCE_API uint32_t jce_skinned_mesh_set_meshlets(JceSkinnedMesh *mesh,
                                               const uint32_t *indices,
                                               uint32_t num_indices,
                                               const uint32_t *desc,
                                               const float *bounds,
                                               const float *errors,
                                               uint32_t count);
JCE_API uint32_t jce_skinned_mesh_meshlet_count(const JceSkinnedMesh *mesh);
JCE_API uint32_t jce_skinned_mesh_meshlet_ibh(const JceSkinnedMesh *mesh);
JCE_API uint32_t jce_skinned_mesh_meshlet_data_vb(const JceSkinnedMesh *mesh);

/* Submit the mesh bound to reduced LOD `level` (0-based) instead of the base
 * index buffer.  level >= lod_count (or no LODs) falls through to
 * jce_skinned_mesh_submit (base geometry).  Wireframe + render state are
 * identical to the base submit; only the index buffer differs.  Caller binds
 * material/program before, and submits the program after, exactly as for the
 * base submit. */
JCE_API void JCE_CALL jce_skinned_mesh_submit_lod(const JceSkinnedMesh *mesh,
                                                  const JceRenderer *r,
                                                  uint16_t view_id,
                                                  uint32_t level);

/* Depth-only mirror of jce_skinned_mesh_submit_lod for the shadow pass: binds
 * reduced LOD `level`'s index buffer so the cast silhouette matches the LOD the
 * color pass drew.  level out of range => jce_skinned_mesh_submit_shadow. */
JCE_API void JCE_CALL jce_skinned_mesh_submit_shadow_lod(const JceSkinnedMesh *mesh,
                                                         const JceRenderer *r,
                                                         uint16_t view_id,
                                                         JceShaderHandle program,
                                                         uint32_t level);

/* ================================================================== */
/* Morph deform support (FEATURE 3.1, opt-in via retain_cpu)            */
/* ================================================================== */
/*
 * When a mesh was created with retain_cpu == true, the following accessors
 * expose the retained, undeformed base vertex array and the exact interleaved
 * layout it was uploaded with, so a per-instance CPU morph deform
 * (jce_morph_apply) can produce a deformed copy and push it into a dynamic
 * vertex buffer that the SAME skinned program then reads.  All return 0 / NULL
 * when retain_cpu was false (the legacy default).  The deform MUST write the
 * SAME stride/layout — the trailing joints/weights bytes are copied through
 * untouched by jce_morph_apply (it only touches pos/normal at the offsets
 * reported here).
 */

/* Retained base vertex bytes (num_verts * stride), or NULL when not retained.
 * Read-only; owned by the mesh. */
JCE_API const void *jce_skinned_mesh_base_verts(const JceSkinnedMesh *mesh);

/* Byte stride between vertices in the (static and dynamic) VB. 0 if not retained. */
JCE_API uint32_t jce_skinned_mesh_stride(const JceSkinnedMesh *mesh);

/* Byte offset of the float[3] POSITION field within each vertex. */
JCE_API uint32_t jce_skinned_mesh_pos_offset(const JceSkinnedMesh *mesh);

/* Byte offset of the float[3] NORMAL field within each vertex. */
JCE_API uint32_t jce_skinned_mesh_normal_offset(const JceSkinnedMesh *mesh);

/* The bgfx_vertex_layout_t the static VB was created with (so the dynamic VB
 * matches exactly).  Returned as const void* to keep this public header
 * bgfx-free; the renderer casts it to const bgfx_vertex_layout_t*.  NULL when
 * not retained. */
JCE_API const void *jce_skinned_mesh_layout(const JceSkinnedMesh *mesh);

JCE_EXTERN_C_END

#endif /* JCE_SKINNED_MESH_H */
