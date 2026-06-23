/*
 * jce_model.h  Public API for loading and drawing composite glTF models.
 *
 * A JceModel encapsulates meshes, PBR materials, an optional skeleton,
 * and animation clips loaded from a GLB/glTF file in the PAK archive.
 *
 * Layer: Graphics (Layer 3) — public.
 */

#ifndef JCE_MODEL_PUBLIC_H
#define JCE_MODEL_PUBLIC_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>
#include <jce/middleware/animation/jce_morph.h>   /* JceMorphData (narrow morph accessors) */

#include <stdint.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

#ifndef JCE_MODEL_FWD_DECLARED
#define JCE_MODEL_FWD_DECLARED
typedef struct JceModel    JceModel;
#endif
typedef struct JceRenderer JceRenderer;
typedef struct JcePakArchive  JcePakArchive;
typedef struct JceSkeleton JceSkeleton;
typedef struct JceAnimClip JceAnimClip;
typedef struct JceSkinnedMesh JceSkinnedMesh;   /* opaque (jce_skinned_mesh.h) */
#ifndef JCE_PBR_MATERIAL_FWD_DECLARED
#define JCE_PBR_MATERIAL_FWD_DECLARED
typedef struct JcePbrMaterial JcePbrMaterial;   /* opaque (jce_pbr_material.h) */
#endif

/* Opaque per-instance dynamic-VB override (morph GPU deform).  The scene
 * renderer owns an array of these and passes the table to jce_model_draw_morphed
 * / _shadow; for each (node, prim) the callback returns the bgfx dynamic vertex
 * buffer that holds the CPU-morphed vertices for THAT instance, or
 * BGFX_INVALID_HANDLE to fall through to the primitive's static skinned VB.
 * The handle is passed as a bare uint16_t (== bgfx_dynamic_vertex_buffer_handle_t
 * .idx) so the public header stays bgfx-free; UINT16_MAX means "no override". */
typedef uint16_t (*JceModelMorphVbCb)(void *user, uint32_t node, uint32_t prim);

/* Load a glTF/GLB model from the PAK archive.
   Returns NULL on failure (asset not found, parse error, OOM). */
JCE_API JceModel *jce_model_load_gltf(const JcePakArchive *pak, const char *asset_path);

/* Load a glTF/GLB model from raw file bytes in memory.
   name is used for logging only; may be NULL. */
JceModel *jce_model_load_gltf_memory(const void *data, uint32_t size,
                                      const char *name);

/* Header-only rig probe: cgltf-parses just the glTF/GLB JSON header (no
   buffer load, image decode, mesh build, or animation sampling) to report
   whether the model carries a skin and/or animation clips.  Cheap enough
   to call on a latency-sensitive path (e.g. the editor's asset drop) where
   a full load would block.  Either out pointer may be NULL.  Returns false
   on parse failure (caller should treat the model as static). */
JCE_API bool jce_model_probe_rig_memory(const void *data, uint32_t size,
                                        bool *out_has_skin, bool *out_has_anim);

/* Worker-decode + render-thread-upload split (see jce_gltf_loader.h).
 *   worker:        JceModelCpu *c = jce_model_decode_gltf_cpu(pak, path);
 *   render thread: JceModel    *m = jce_model_upload_gltf_cpu(c);  // consumes c
 *   cancel:        jce_model_gltf_cpu_free(c);                     // no upload
 * decode_cpu does cgltf parse + CPU extraction + image decode (no bgfx);
 * upload_cpu creates the GPU meshes + textures on the calling thread. */
#ifndef JCE_MODELCPU_FWD_DECLARED
#define JCE_MODELCPU_FWD_DECLARED
typedef struct JceModelCpu JceModelCpu;
#endif
JCE_API JceModelCpu *jce_model_decode_gltf_cpu(const JcePakArchive *pak,
                                               const char *asset_path);
JCE_API JceModel    *jce_model_upload_gltf_cpu(JceModelCpu *cpu);
JCE_API void         jce_model_gltf_cpu_free(JceModelCpu *cpu);

/* Destroy a model and all owned GPU resources (meshes, textures, skeleton). */
JCE_API void jce_model_destroy(JceModel *model);

/* Pre-submit bind hook.  jce_model_draw issues one bgfx_submit per primitive
 * and bgfx clears the per-draw texture-stage + uniform state between submits.
 * A caller that must add a per-submit bind (e.g. the scene renderer's Forward+
 * cluster texture on stage 14, which the fs_pbr_fwdplus variant reads) arms
 * this hook; it fires immediately BEFORE each color-pass submit, so every
 * primitive of a skinned/LOD model gets the bind (a single bind before the
 * call would only cover the first primitive).  Render-thread only.  Pass NULL
 * to clear; default unset => no-op => byte-identical to the unhooked path.
 * NOT called by jce_model_draw_shadow (depth-only, no materials). */
#ifndef JCE_MODEL_PRESUBMIT_CB_DEFINED
#define JCE_MODEL_PRESUBMIT_CB_DEFINED
typedef void (*JceModelPreSubmitCb)(void *user, uint16_t view_id);
#endif
void jce_model_set_pre_submit_cb(JceModelPreSubmitCb cb, void *user);

/* Per-draw material override.  When `mat` is non-NULL, the NEXT jce_model_draw /
 * _draw_morphed calls bind it for EVERY primitive instead of the model's own
 * embedded material — the standard-engine behaviour where a renderer's assigned
 * material slot overrides the imported mesh's default materials.  The scene
 * renderer arms this for a MeshRenderer that authored a material/albedo texture
 * (so a geometry-only model converted to glTF still shows its scene-assigned
 * texture) and clears it (NULL) right after the draw.  Render-thread only;
 * default NULL => use embedded materials (byte-identical to before). */
JCE_API void jce_model_set_material_override(const JcePbrMaterial *mat);

/* Editor missing-albedo checker fallback.  When `on`, the NEXT jce_model_draw
 * renders any primitive whose effective material has no valid albedo texture
 * with the pink-black checker (the editor's missing-asset hint), matching the
 * simple-mesh path so glTF models behave the same in TEXTURED / SHADED view
 * modes.  Scene renderer arms it for editor model draws and clears it (false)
 * after.  Default off => no checker (runtime never shows it). */
JCE_API void jce_model_set_albedo_checker(bool on);

/* Draw all mesh primitives with their PBR materials.
 *
 * transform:      model-to-world matrix applied to every node (required).
 * joint_matrices: bone palette for skinned meshes; pass NULL for static models.
 * num_joints:     number of matrices in joint_matrices (0 for static models). */
void jce_model_draw(const JceModel *model,
                    const JceRenderer *r, uint16_t view_id,
                    const jce_mat4 *transform,
                    const jce_mat4 *joint_matrices,
                    uint32_t num_joints);

/* True when the model has no truly-skinned primitives, so it can be drawn with
 * jce_model_draw_instanced (a skinned primitive needs per-instance bone
 * palettes and cannot share one instanced submit). */
JCE_API bool jce_model_is_instanceable(const JceModel *model);

/* GPU-instanced color draw: render `count` copies of a non-skinned model, one
 * per roots[i] model-to-world transform, batching each primitive into a single
 * instanced submit (PBR material bound once per primitive, shared across all
 * instances) via the instanced PBR program.  Cuts per-frame uniform writes
 * ~Nx for repeated meshes — the key to dense streamed scenes staying under
 * bgfx's fixed VK uniform scratch.  Falls back to per-instance jce_model_draw
 * when count==1 or the instanced program is unavailable. */
JCE_API void jce_model_draw_instanced(const JceModel *model,
                                      const JceRenderer *r, uint16_t view_id,
                                      const jce_mat4 *roots, uint32_t count);

/* GPU-driven instancing helper (roadmap #18, Phase 0+1).  Returns true when the
 * model has EXACTLY ONE drawable, non-skinned, non-joint-parented primitive — the
 * case the GPU-driven color path handles (one world matrix per resident
 * instance).  Writes that primitive's pre-baked node-local (model-space world)
 * transform to *out_node_lt (when non-NULL) so the caller can fold it into each
 * instance's world matrix before uploading to the GPUScene.  Multi-primitive /
 * multi-node / skinned / joint-parented models return false → CPU path. */
JCE_API bool jce_model_gpu_instanceable(const JceModel *model,
                                        jce_mat4 *out_node_lt);

/* GPU-driven instanced color draw: submits the single drawable primitive once,
 * sourcing per-instance model matrices from the compute-written visible dynamic
 * vertex buffer `visible_vb` (a bgfx dynamic_vertex_buffer handle index; mat4
 * per slot, byte-compatible with vs_pbr_inst's i_data0..3), starting at slot
 * `start` for `count` instances (this run's partition).  `count` is the upper-
 * bound instance count (the GPU cull leaves the partition tail zeroed →
 * degenerate).  No-op unless jce_model_gpu_instanceable(model, NULL) would
 * return true. */
JCE_API void jce_model_draw_instanced_from_buffer(const JceModel *model,
                                                  const JceRenderer *r,
                                                  uint16_t view_id,
                                                  uint16_t visible_vb,
                                                  uint32_t start,
                                                  uint32_t count);

/* Draw all primitives into a shadow/depth pass (depth-only, no materials).
 *
 * Skinned primitives reuse the same world-space bone palette the color
 * pass uploads, so an animated model casts a shadow that follows its
 * skeleton.  Pass NULL/0 joints to rasterize the bind pose.
 *
 * view_id must be a shadow producer view (single map or a CSM cascade). */
void jce_model_draw_shadow(const JceModel *model,
                           const JceRenderer *r, uint16_t view_id,
                           const jce_mat4 *transform,
                           const jce_mat4 *joint_matrices,
                           uint32_t num_joints);

/* GPU-instanced depth-only sibling of jce_model_draw_shadow: rasterizes the
 * static (non-skinned) primitives for `count` world matrices in one submit per
 * primitive via the shadow_inst program.  Collapses a shadow cascade's repeated
 * meshes to ~unique-mesh draws.  Skinned primitives are skipped (draw those
 * per-entity).  Falls back to per-instance jce_model_draw_shadow when count==1
 * or the instanced program is unavailable. */
JCE_API void jce_model_draw_shadow_instanced(const JceModel *model,
                                             const JceRenderer *r, uint16_t view_id,
                                             const jce_mat4 *roots, uint32_t count);

/* Morph-aware sibling of jce_model_draw / jce_model_draw_shadow (FEATURE 3.1
 * GPU vertex-deform).  Identical to the base entrypoints EXCEPT that, for each
 * skinned primitive, the renderer first asks `vb_cb(user, node, prim)` for a
 * per-instance dynamic vertex buffer holding that instance's CPU-morphed
 * vertices.  When the callback returns a valid handle (idx != UINT16_MAX) the
 * dynamic VB is bound in place of the primitive's static skinned VB before
 * submit; otherwise the draw falls through to the EXACT existing static path.
 * The bone palette, u_model[] upload, and shaders are untouched — morph is a
 * pure pre-skin vertex rewrite, so the same skinned program reads the deformed
 * VB.  A NULL vb_cb makes these byte-identical to jce_model_draw / _shadow.
 * The color and shadow variants MUST be driven by the SAME callback/handles so
 * the cast silhouette matches the lit, morphed mesh. */
void jce_model_draw_morphed(const JceModel *model,
                            const JceRenderer *r, uint16_t view_id,
                            const jce_mat4 *transform,
                            const jce_mat4 *joint_matrices,
                            uint32_t num_joints,
                            JceModelMorphVbCb vb_cb, void *vb_user);

void jce_model_draw_morphed_shadow(const JceModel *model,
                                   const JceRenderer *r, uint16_t view_id,
                                   const jce_mat4 *transform,
                                   const jce_mat4 *joint_matrices,
                                   uint32_t num_joints,
                                   JceModelMorphVbCb vb_cb, void *vb_user);

/* Submit a wireframe overlay of every primitive in the model, walking
 * the full node hierarchy. Used by editor tooling (selection outlines)
 * to draw the true geometric silhouette of skinned/static models that
 * already had their materials/lights bound by the regular pass.
 *
 * Pass joint_matrices == NULL for the bind pose (skinned meshes only);
 * otherwise pass the live bone palette so the wireframe deforms with
 * the current animation. */
JCE_API void jce_model_submit_wireframe_overlay(const JceModel *model,
                                                const JceRenderer *r,
                                                uint16_t view_id,
                                                const jce_mat4 *transform,
                                                const jce_mat4 *joint_matrices,
                                                uint32_t num_joints);

/* Submit every primitive to an object-ID picking pass.
 * Caller must set the object-ID uniform before calling.  The function walks
 * the same node hierarchy as jce_model_draw() and respects per-material
 * double-sided culling. */
JCE_API void jce_model_submit_pick_id(const JceModel *model,
                                      const JceRenderer *r,
                                      uint16_t view_id,
                                      const jce_mat4 *transform,
                                      const jce_mat4 *joint_matrices,
                                      uint32_t num_joints,
                                      JceShaderHandle static_program,
                                      JceShaderHandle skinned_program);

/* -- TAA per-object / per-bone motion vectors ----------------------- */
/*
 * Context for jce_model_draw_velocity: the static + skinned velocity programs
 * and the bgfx uniform handle indices (.idx values; model.c rebuilds the
 * handles) the velocity shaders consume.  Matrices are supplied per-frame:
 *   cur_view_proj  = THIS frame's UN-jittered view*proj
 *   prev_view_proj = PREVIOUS frame's UN-jittered view*proj
 * Per primitive the draw sets the current world bone palette into u_model[]
 * (as the color/shadow pass does) and the previous world palette into
 * u_prevBones[] (skinned) / u_prevModel (static), so the velocity shader can
 * project both and write (curNDC - prevNDC)*0.5+0.5 (matching fs_motion_vec.sc).
 */
typedef struct {
    JceShaderHandle static_program;   /* vs_gbuffer_vel + fs_gbuffer_vel */
    JceShaderHandle skinned_program;  /* vs_gbuffer_vel_skinned + fs_gbuffer_vel_skinned */
    uint16_t        u_prev_model_idx; /* uniform mat4 (static prev world) */
    uint16_t        u_prev_bones_idx; /* uniform mat4[JCE_MAX_BONES] (skinned prev) */
    uint16_t        u_cur_vp_idx;     /* uniform mat4 (cur un-jittered view*proj) */
    uint16_t        u_prev_vp_idx;    /* uniform mat4 (prev un-jittered view*proj) */
    jce_mat4        cur_view_proj;
    jce_mat4        prev_view_proj;
} JceModelVelocityCtx;

/* Draw all primitives into the per-object motion-vector G-buffer pass.
 * Mirrors jce_model_draw_shadow's node/skin walk, but binds the velocity
 * programs and the prev/cur matrices in `ctx`.
 *   transform / joint_matrices / num_joints = THIS frame's world + skin palette
 *   prev_transform                          = PREVIOUS frame's world (NULL =
 *                                             reuse `transform` => zero object
 *                                             motion, camera motion still shows)
 *   prev_joint_matrices / num_prev_joints   = PREVIOUS frame's skin palette
 *                                             (NULL/0 => reuse current => bones
 *                                             contribute no motion this frame) */
void jce_model_draw_velocity(const JceModel *model,
                             const JceRenderer *r, uint16_t view_id,
                             const JceModelVelocityCtx *ctx,
                             const jce_mat4 *transform,
                             const jce_mat4 *joint_matrices,
                             uint32_t num_joints,
                             const jce_mat4 *prev_transform,
                             const jce_mat4 *prev_joint_matrices,
                             uint32_t num_prev_joints);

/* -- Skeleton & animation accessors -------------------------------- */

/* Returns the skeleton, or NULL if the model has no skinning. */
JCE_API JceSkeleton  *jce_model_get_skeleton(const JceModel *model);

/* Local-space (model-space) axis-aligned bounding box, computed once at upload
 * from every primitive's vertex positions (node transforms baked in).  Writes
 * out_min[3]/out_max[3] and returns true when the model carries valid bounds;
 * returns false (out_* untouched) for an empty/boundless model.  Used by the
 * scene renderer's frustum cull so model-path entities cull by their real
 * extent rather than a point at the origin. */
JCE_API bool          jce_model_get_aabb(const JceModel *model,
                                         float out_min[3], float out_max[3]);

/* Number of animation clips embedded in the model. */
JCE_API uint32_t      jce_model_anim_count(const JceModel *model);

/* Get animation clip by index. Returns NULL if out of range. */
JCE_API JceAnimClip  *jce_model_get_anim(const JceModel *model, uint32_t index);

/* -- Morph targets / blendshapes (FEATURE 3.1) ---------------------- */
/* Narrow morph-walk API: lets a renderer enumerate (node, prim) pairs, learn
 * vertex counts, and fetch per-prim morph deltas WITHOUT exposing the internal
 * JceModelNode / JceModelPrimitive / JceMesh / JceSkinnedMesh struct layouts.
 * Signatures are kept bit-identical to the internal engine/src/renderer
 * jce_model.h declarations (the internal header re-uses these). */

/* Number of nodes in the model's scene hierarchy. */
JCE_API uint32_t      jce_model_node_count(const JceModel *model);

/* Number of draw primitives under a node (0 if node out of range). */
JCE_API uint32_t      jce_model_node_prim_count(const JceModel *model,
                                                uint32_t node);

/* Vertex count of one (node, prim) primitive's mesh — the count the morph
 * deltas were authored against.  0 if (node, prim) is out of range or the
 * primitive carries no mesh.  Used to LOD-guard the deform (only deform when
 * the bound mesh's vertex count equals the morph target vertex count). */
JCE_API uint32_t      jce_model_prim_vertex_count(const JceModel *model,
                                                  uint32_t node, uint32_t prim);

/* The (opaque) skinned mesh of one (node, prim) primitive, or NULL if the
 * primitive carries no skinned/PBR mesh (or indices out of range).  The morph
 * deform reads its retained base verts / stride / layout via the
 * jce_skinned_mesh_* accessors; the struct layout stays hidden. */
JCE_API const JceSkinnedMesh *jce_model_prim_skinned_mesh(const JceModel *model,
                                                          uint32_t node,
                                                          uint32_t prim);

/* Morph deltas for a primitive (node, prim), or NULL if it has none.  The
 * returned JceMorphData is owned by the model.  Mirrors the internal accessor
 * so a renderer that includes only this public header can detect morph-bearing
 * primitives and feed jce_morph_apply. */
JCE_API const JceMorphData *jce_model_prim_morph(const JceModel *model,
                                                 uint32_t node, uint32_t prim);

JCE_EXTERN_C_END

#endif /* JCE_MODEL_PUBLIC_H */
