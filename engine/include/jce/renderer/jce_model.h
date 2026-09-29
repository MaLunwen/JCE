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
typedef struct JceMesh     JceMesh;             /* opaque (jce_mesh.h) */
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
JCE_API JceModel *jce_model_load_gltf_memory(const void *data, uint32_t size,
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
/* Decode glTF/GLB bytes to the same CPU-only intermediate. `name` is used
 * for logging and resolving external buffers. Safe on background workers. */
JCE_API JceModelCpu *jce_model_decode_gltf_cpu_memory(const void *data,
                                                       uint32_t size,
                                                       const char *name);
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
JCE_API void jce_model_set_pre_submit_cb(JceModelPreSubmitCb cb, void *user);

/* Per-draw material override.  When `mat` is non-NULL, the NEXT jce_model_draw /
 * _draw_morphed calls bind it for EVERY primitive instead of the model's own
 * embedded material — the standard-engine behaviour where a renderer's assigned
 * material slot overrides the imported mesh's default materials.  The scene
 * renderer arms this for a MeshRenderer that authored a material/albedo texture
 * (so a geometry-only model converted to glTF still shows its scene-assigned
 * texture) and clears it (NULL) right after the draw.  Render-thread only;
 * default NULL => use embedded materials (byte-identical to before). */
JCE_API void jce_model_set_material_override(const JcePbrMaterial *mat);

/* Current per-draw material override (NULL when none).  Render-thread only;
 * used by the tinted instanced solo fallback to compose a per-instance tint
 * over the effective material. */
JCE_API const JcePbrMaterial *jce_model_get_material_override(void);

/* Editor missing-albedo checker fallback.  When `on`, the NEXT jce_model_draw
 * renders any primitive whose effective material has no valid albedo texture
 * with the pink-black checker (the editor's missing-asset hint), matching the
 * simple-mesh path so glTF models behave the same in TEXTURED / SHADED view
 * modes.  Scene renderer arms it for editor model draws and clears it (false)
 * after.  Default off => no checker (runtime never shows it). */
JCE_API void jce_model_set_albedo_checker(bool on);

/* In-asset auto-LOD selector for the NEXT model draw (large-world-opt P1 #6).
 * level 0 = base geometry (LOD0, the default).  level >= 1 binds the (level-1)'th
 * reduced index buffer on each non-skinned primitive's mesh (cooked into the GLB
 * via the JCE_lod extension); a primitive without that many LODs falls through to
 * its base index buffer, and skinned rigs always keep full detail.  Applies to
 * jce_model_draw / _draw_instanced / _draw_shadow / _draw_shadow_instanced (so a
 * mid-distance object's shadow casts the SAME LOD it renders).  Render-thread
 * only (mirrors jce_model_set_material_override); the scene renderer arms it from
 * jce_lod_pick, draws, and resets to 0.  level 0 => byte-identical to pre-LOD. */
JCE_API void jce_model_set_draw_lod(uint32_t level);

/* Largest in-asset reduced-LOD count across the model's non-skinned primitives
 * (0 = no in-asset LODs).  Lets the renderer clamp a LODGroup level to the
 * geometry actually shipped, and the inspector report the cooked chain depth. */
JCE_API uint32_t jce_model_max_lod(const JceModel *model);

/* Index counts of the model's first drawable non-skinned primitive: out_base
 * receives the LOD0 index count, out_lods[i] (0-based, up to max_levels) the
 * i'th reduced level's index count.  Returns the number of reduced levels
 * written.  For the inspector's per-level triangle readout (tris = count / 3).
 * Any out pointer may be NULL. */
JCE_API uint32_t jce_model_lod_index_counts(const JceModel *model,
                                            uint32_t *out_base,
                                            uint32_t *out_lods,
                                            uint32_t max_levels);

/* Draw all mesh primitives with their PBR materials.
 *
 * transform:      model-to-world matrix applied to every node (required).
 * joint_matrices: bone palette for skinned meshes; pass NULL for static models.
 * num_joints:     number of matrices in joint_matrices (0 for static models). */
JCE_API void jce_model_draw(const JceModel *model,
                    const JceRenderer *r, uint16_t view_id,
                    const jce_mat4 *transform,
                    const jce_mat4 *joint_matrices,
                    uint32_t num_joints);

/* KEYSTONE (stylized-slice §5.6): jce_model_draw with a per-call override for
 * the TRULY-SKINNED color program only.  When skinned_color_override is valid
 * (.idx != UINT16_MAX) it replaces jce_renderer_get_program_pbr_skinned for
 * every fully-skinned primitive; the non-skinned (PBR-static) branch and every
 * other behaviour are UNCHANGED.  Passing JCE_INVALID_SHADER is BYTE-IDENTICAL
 * to jce_model_draw (which is now a thin caller of this).  Used by the scene
 * renderer to draw a toon character with the pbr_toon program. */
/* Arm index RUNS for the next jce_model_draw*: every primitive is submitted
 * once per run instead of once for its whole index buffer.  Cleared by the
 * draw, like jce_model_set_draw_lod beside it.
 *
 * FOR STATIC-BATCH MEMBER CULLING.  A merged group is one draw -- the point of
 * merging -- and therefore ONE cullable object: a row of forty fence posts
 * draws all forty whenever any one is on screen.  The member table
 * (jce_static_batch.h) says which index range each original mesh occupies, so
 * the caller submits the runs it can see, and a wholly visible group coalesces
 * to ONE run and takes exactly the path it always took.
 *
 * ARMED AT THE BOTTOM, and that is the whole reason this exists rather than a
 * check at a call site: a static model reaches a draw through several ladders
 * with several exits, and four wirings that each looked right reached nothing
 * because they guessed the wrong one.  Here the caller does not have to know.
 *
 * n == 0 disarms.  Runs apply to a model with EXACTLY ONE primitive -- a
 * merged group is one -- and are ignored otherwise, because a range into
 * primitive 0's buffer means nothing in primitive 1's. */
JCE_API void jce_model_set_draw_index_runs(const uint32_t *first,
                                           const uint32_t *count,
                                           uint32_t        n);

JCE_API void jce_model_draw_program(const JceModel *model,
                            const JceRenderer *r, uint16_t view_id,
                            const jce_mat4 *transform,
                            const jce_mat4 *joint_matrices,
                            uint32_t num_joints,
                            JceShaderHandle skinned_color_override);

/* True when the model has no truly-skinned primitives, so it can be drawn with
 * jce_model_draw_instanced (a skinned primitive needs per-instance bone
 * palettes and cannot share one instanced submit). */
JCE_API bool jce_model_is_instanceable(const JceModel *model);

/* True when EVERY drawable primitive is skinned (dual of _is_instanceable): the
 * model has no static / non-skinned sub-mesh.  The bind-pose instancing paths
 * (color + shadow) draw ONLY skinned primitives, so they gate on this — a mixed
 * skinned+static rig falls through to the per-primitive path that also draws /
 * casts its static parts. */
JCE_API bool jce_model_is_purely_skinned(const JceModel *model);

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

/* Tint-aware sibling of jce_model_draw_instanced (large-world-opt P1 #7): each
 * instance i additionally carries tints[i] (RGBA, linear) packed as a 5th
 * per-instance vec4 (i_data4) and modulating albedo in fs_pbr_tint exactly like
 * a solo draw's u_baseColorFactor.  This lets baseColor-only copies of one mesh
 * COLLAPSE into a single instanced submit instead of falling back to solo draws.
 * tints == NULL is byte-identical to jce_model_draw_instanced (stride-64 buffer,
 * plain vs_pbr_inst).  When tints != NULL but the tint program is unavailable
 * (older pak) it falls back to the no-tint program (tint dropped, no crash). */
JCE_API void jce_model_draw_instanced_tinted(const JceModel *model,
                                             const JceRenderer *r,
                                             uint16_t view_id,
                                             const jce_mat4 *roots,
                                             const jce_vec4 *tints,
                                             uint32_t count);

/* 千万 S4/S5 LOD-in-cull draw: the GPU cull has already compacted survivors
 * into the band PARTITIONS of `visible_vb` and built one drawIndexedIndirect
 * element per band in `indirect_buf` (element b's startInstance = the band's
 * partition base).  Binds the single drawable primitive's material and issues
 * one submit_indirect per band at that band's reduced LOD index buffer (bands
 * share one vertex buffer).  Band 0 = base LOD, band b>=1 = reduced level
 * (b-1), clamped.  Single-primitive foliage models only; prog = the cross-fade
 * instanced program (vs_pbr_inst_fade) or plain vs_pbr_inst.  The global PBR
 * (lights/IBL/shadows) + forward+ must be bound by the caller. */
JCE_API void jce_model_draw_foliage_lod_indirect(
        const JceModel *model, const JceRenderer *r, uint16_t view_id,
        uint16_t prog_idx, uint32_t band_count,
        uint16_t visible_vb, uint16_t indirect_buf);

/* GPU crowd instancing: draw `count` uniquely-posed instances of a SKINNED
 * model in one instanced submit per skinned primitive.  Per-instance stream:
 * world matrix (i_data0..3) + bone PALETTE BASE in bones (i_data4.x) into the
 * shared per-frame bone texture, bound at sampler stage 4 — ALIASING s_emissive
 * (all 16 stages are otherwise occupied; 9-12 belong to the CSM cascades, which
 * the pre-submit hook re-binds after any earlier bind).  Callers must therefore
 * exclude emissive materials (jce_model_any_emissive) so the aliased sample is
 * shading-neutral.  prog must be vs_pbr_skinned_inst + fs_pbr; the palette is
 * skeleton-local (world applied per instance).  Raw idx handles keep bgfx out
 * of this header. */
JCE_API void jce_model_draw_crowd_instanced(const JceModel *model,
                                            const JceRenderer *r,
                                            uint16_t view_id, uint16_t prog_idx,
                                            const jce_mat4 *worlds,
                                            const uint32_t *bases, uint32_t count,
                                            uint16_t s_bones_idx,
                                            uint16_t bone_tex_idx,
                                            uint16_t u_params_idx,
                                            float tex_w, float tex_h);

/* GPU bind-pose instancing: draw `count` NON-animating skinned instances of a
 * model in one instanced submit per skinned primitive, via the plain instanced
 * PBR program (vs_pbr_inst) + per-instance world matrix — no bone palette (a
 * bind-pose skinned mesh is effectively static).  Collapses the bind-pose
 * majority of a large crowd. */
JCE_API void jce_model_draw_bindpose_instanced(const JceModel *model,
                                               const JceRenderer *r,
                                               uint16_t view_id,
                                               const jce_mat4 *worlds,
                                               uint32_t count);

/* Depth-only sibling of jce_model_draw_bindpose_instanced: casts the same
 * NON-animating skinned instances into a shadow view in one instanced submit
 * per skinned primitive, via the instanced shadow program (vs_shadow_inst) —
 * depth-only, no material.  Collapses the per-char skinned shadow submits that
 * dominate a crowd's cascade gather (the #1 sh_gather cost). */
JCE_API void jce_model_draw_bindpose_shadow_instanced(const JceModel *model,
                                                      const JceRenderer *r,
                                                      uint16_t view_id,
                                                      const jce_mat4 *worlds,
                                                      uint32_t count);

/* ANIMATED sibling: casts `count` uniquely-POSED skinned instances into a
 * shadow view in one instanced submit per skinned primitive.  Per-instance
 * stream mirrors jce_model_draw_crowd_instanced (world i_data0..3 + palette
 * base i_data4.x into the shared bone texture, sampler stage 4).  prog must be
 * vs_shadow_skinned_inst + fs_shadow; raw idx handles keep bgfx out of here. */
JCE_API void jce_model_draw_crowd_shadow_instanced(const JceModel *model,
                                                   const JceRenderer *r,
                                                   uint16_t view_id,
                                                   uint16_t prog_idx,
                                                   const jce_mat4 *worlds,
                                                   const uint32_t *bases,
                                                   uint32_t count,
                                                   uint16_t s_bones_idx,
                                                   uint16_t bone_tex_idx,
                                                   uint16_t u_params_idx,
                                                   float tex_w, float tex_h);


/* Instanced draw of a SINGLE static mesh with a per-instance RGBA tint stream
 * (i_data4) — the mesh-level sibling of jce_model_draw_instanced_tinted, for the
 * scene renderer's factor-only primitive tint-instancing: shape primitives that
 * share one built-in mesh + one material signature and differ only in base_color
 * COLLAPSE into one instanced submit instead of one solo draw each.  `state` is
 * the shared bgfx render state (blend/cull/depth).  `pre_submit` (if non-NULL) is
 * invoked once per instance-buffer chunk immediately before the submit to bind
 * the shared material + frame-global light/shadow/IBL state (e.g. the renderer's
 * sr_bind_material_cb) — the tint program modulates the bound base-color factor
 * by v_tint, so binding a WHITE base material yields per-instance colours.
 * tints == NULL falls back to the plain (stride-64) instanced program. */
/* THE SAME DRAW, TOLD WHAT THE MATERIAL ASKS FOR.
 *
 * `shader_keys` is an OR of JCE_SHADER_KEY_* -- normally
 * jce_pbr_material_shader_keys(&mat) -- and is handed to
 * jce_renderer_get_program_variant instead of taking whatever the frame's
 * default instanced program happens to be.
 *
 * It is a separate entry point rather than a parameter on the one above
 * because that one is ABI.  It exists because built-in PRIMITIVES batch
 * through here, and that is the seventh draw path: the previous attempt at a
 * keyword axis wired the other six, forced its variant on every draw, and
 * moved ZERO pixels.  A path that cannot be told what the material wants is
 * a path the axis silently does not reach. */
JCE_API void jce_mesh_draw_instanced_tinted_keyed(const JceMesh *mesh,
                                            const JceRenderer *r,
                                            uint16_t view_id,
                                            const jce_mat4 *worlds,
                                            const jce_vec4 *tints,
                                            uint32_t count,
                                            uint64_t state,
                                            void (*pre_submit)(void *user, uint16_t view_id),
                                            void *pre_submit_user,
                                                  uint32_t shader_keys);

JCE_API void jce_mesh_draw_instanced_tinted(const JceMesh *mesh,
                                            const JceRenderer *r,
                                            uint16_t view_id,
                                            const jce_mat4 *worlds,
                                            const jce_vec4 *tints,
                                            uint32_t count,
                                            uint64_t state,
                                            void (*pre_submit)(void *user, uint16_t view_id),
                                            void *pre_submit_user);

/* GPU-driven instancing helper (roadmap #18, Phase 0+1).  Returns true when the
 * model has EXACTLY ONE drawable, non-skinned, non-joint-parented primitive — the
 * case the GPU-driven color path handles (one world matrix per resident
 * instance).  Writes that primitive's pre-baked node-local (model-space world)
 * transform to *out_node_lt (when non-NULL) so the caller can fold it into each
 * instance's world matrix before uploading to the GPUScene.  Multi-primitive /
 * multi-node / skinned / joint-parented models return false → CPU path. */
JCE_API bool jce_model_gpu_instanceable(const JceModel *model,
                                        jce_mat4 *out_node_lt);

/* Number of drawable primitives jce_model_gpu_instanceable would submit (>1 = a
 * shared-node multi-primitive model, drawn as one GPU cull run + one indirect draw
 * PER primitive).  0 when not GPU-instanceable. */
JCE_API uint32_t jce_model_gpu_drawable_count(const JceModel *model);

/* Index count of the `which`-th drawable primitive (0-based).  Fills that
 * primitive's GPU indirect draw args (numIndices).  0 if out of range. */
JCE_API uint32_t jce_model_gpu_primitive_index_count(const JceModel *model,
                                                     uint32_t which);

/* Static mesh of the `which`-th drawable primitive (same walk order), for CPU-side
 * instancing paths that need the raw VB/IB (e.g. the velocity/depth prepass render
 * queue).  NULL if out of range or the primitive isn't a static mesh. */
JCE_API JceMesh *jce_model_gpu_primitive_mesh(const JceModel *model, uint32_t which);

/* Non-skinned SkinnedMesh (the LOD carrier) of the `which`-th drawable primitive
 * — holds the per-LOD reduced index buffers (jce_skinned_mesh_lod_ibh) the GPU
 * LOD-in-cull path (千万 S4) binds per distance band while sharing one vertex
 * buffer.  NULL when `which` is out of range or that primitive has no static-LOD
 * skinned mesh. */
JCE_API const JceSkinnedMesh *jce_model_gpu_primitive_skinned_mesh(
    const JceModel *model, uint32_t which);

/* SKINNED primitive walk (the complement of the gpu_* static walk) — raw
 * buffers of the model's `which`-th skinned primitive, so a BIND-POSE crowd
 * character can ride the render-queue instancing paths (velocity prepass) as
 * if static: bind-pose vertices are model-space, and the instanced programs
 * ignore the bone attributes.  Returns false when out of range. */
/* True when ANY of the model's materials has an active emissive term (map or
 * non-zero factor).  The GPU crowd-instancing path aliases the s_emissive
 * sampler stage for its bone-palette texture (all 16 stages are otherwise
 * occupied); emissive models are excluded from the batch (per-char path) so
 * the aliased sample can never contribute to shading (fs multiplies it by the
 * factor, which the gate guarantees is zero). */
JCE_API bool jce_model_any_emissive(const JceModel *model);

JCE_API uint32_t jce_model_skinned_primitive_count(const JceModel *model);
JCE_API bool jce_model_skinned_primitive_buffers(const JceModel *model,
                                                 uint32_t which,
                                                 uint32_t *out_vbh,
                                                 uint32_t *out_ibh,
                                                 uint32_t *out_index_count);

/* GPU-driven instanced color draw: submits the `which`-th drawable primitive once,
 * sourcing per-instance model matrices from the compute-written visible dynamic
 * vertex buffer `visible_vb` (a bgfx dynamic_vertex_buffer handle index; mat4
 * per slot, byte-compatible with vs_pbr_inst's i_data0..3), starting at slot
 * `start` for `count` instances (this partition).  `count` is the upper-bound
 * instance count (the GPU cull leaves the partition tail zeroed → degenerate).
 * `which` = 0 for a single-primitive model; 0..N-1 across a shared-node
 * multi-primitive model.  No-op unless jce_model_gpu_instanceable. */
JCE_API void jce_model_draw_instanced_from_buffer(const JceModel *model,
                                                  const JceRenderer *r,
                                                  uint16_t view_id,
                                                  uint16_t visible_vb,
                                                  uint32_t start,
                                                  uint32_t count,
                                                  uint32_t which);

/* Index count of the single drawable primitive (the one jce_model_gpu_instanceable
 * accepts).  Used to fill the GPU indirect draw args (numIndices).  Returns 0
 * when the model is not GPU-instanceable. */
JCE_API uint32_t jce_model_gpu_index_count(const JceModel *model);

/* GPU-driven INDIRECT instanced color draw (roadmap #18, Direction C).  Binds the
 * single drawable primitive's VB/IB + material and the compute-compacted visible
 * instance buffer `visible_vb` (from slot 0), then issues ONE bgfx_submit_indirect
 * reading this run's draw args from `indirect_buf` element `indirect_el`.  The GPU
 * filled numInstances = survivor count and startInstance = the run's partition
 * base, so only survivors rasterise (no degenerate instances) and no CPU per-run
 * fixed count is needed.  No-op unless jce_model_gpu_instanceable(model, NULL). */
JCE_API void jce_model_draw_indirect_from_buffer(const JceModel *model,
                                                 const JceRenderer *r,
                                                 uint16_t view_id,
                                                 uint16_t visible_vb,
                                                 uint16_t indirect_buf,
                                                 uint32_t indirect_el,
                                                 uint32_t which);

/* Draw all primitives into a shadow/depth pass (depth-only, no materials).
 *
 * Skinned primitives reuse the same world-space bone palette the color
 * pass uploads, so an animated model casts a shadow that follows its
 * skeleton.  Pass NULL/0 joints to rasterize the bind pose.
 *
 * view_id must be a shadow producer view (single map or a CSM cascade). */
JCE_API void jce_model_draw_shadow(const JceModel *model,
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

/* GPU-driven DEPTH-ONLY siblings of jce_model_draw_*_from_buffer (roadmap #18
 * extended to the CSM cascades): submit the single drawable static primitive once
 * with the SHADOW instanced program (`program_idx`, from
 * jce_renderer_get_program_shadow_inst) and depth-only state, sourcing per-instance
 * matrices from the compute-written visible dynamic vertex buffer `visible_vb`.
 * The instanced variant uses a CPU start/count over the run's partition; the
 * indirect variant reads count/start from the GPU-built `indirect_buf` element
 * `indirect_el`.  No material bind, no Forward+ pre-submit.  No-op unless
 * jce_model_gpu_instanceable(model, NULL) would return true. */
JCE_API void jce_model_draw_shadow_instanced_from_buffer(const JceModel *model,
                                                         const JceRenderer *r,
                                                         uint16_t view_id,
                                                         uint16_t program_idx,
                                                         uint16_t visible_vb,
                                                         uint32_t start,
                                                         uint32_t count,
                                                         uint32_t which);
JCE_API void jce_model_draw_shadow_indirect_from_buffer(const JceModel *model,
                                                        const JceRenderer *r,
                                                        uint16_t view_id,
                                                        uint16_t program_idx,
                                                        uint16_t visible_vb,
                                                        uint16_t indirect_buf,
                                                        uint32_t indirect_el,
                                                        uint32_t which);

/* Nanite-lite V2: one submit_indirect over the meshlet-grouped index buffer
 * (per-meshlet args written by cs_meshlet_cull; culled = zero-index).  Binds
 * prim 0's material + the entity world transform; prog = the solo pbr
 * program.  Global PBR state must already be bound by the caller. */
JCE_API void jce_model_draw_meshlet_culled(const JceModel *model,
                                           const JceRenderer *r,
                                           uint16_t view_id, uint16_t prog_idx,
                                           const jce_mat4 *world,
                                           uint16_t indirect_buf,
                                           uint32_t count);

/* Nanite-lite V4: depth-only meshlet-culled draw into a shadow cascade view.
 * Same per-cluster indirect args (from a shadow-mode cull against the light
 * frustum), but the static depth-only shadow program + shadow depth state and
 * no material bind. */
JCE_API void jce_model_draw_meshlet_culled_shadow(const JceModel *model,
                                                  const JceRenderer *r,
                                                  uint16_t view_id,
                                                  const jce_mat4 *world,
                                                  uint16_t indirect_buf,
                                                  uint32_t count);

/* Scatter-shadow instanced submit (千万 ③): one depth-only instanced draw of
 * the model's single drawable primitive at reduced in-asset LOD `level`
 * (clamped to the coarsest available; UINT32_MAX = coarsest), with instance
 * matrices sourced from a persistent dynamic VB (raw idx, e.g. the scatter's
 * COMPUTE_READ roots buffer doubling as plain instance data). */
JCE_API void jce_model_draw_shadow_instanced_lod(const JceModel *model,
                                                 const JceRenderer *r,
                                                 uint16_t view_id,
                                                 uint16_t program_idx,
                                                 uint16_t inst_vb,
                                                 uint32_t count,
                                                 uint32_t level);

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
JCE_API void jce_model_draw_morphed(const JceModel *model,
                            const JceRenderer *r, uint16_t view_id,
                            const jce_mat4 *transform,
                            const jce_mat4 *joint_matrices,
                            uint32_t num_joints,
                            JceModelMorphVbCb vb_cb, void *vb_user);

JCE_API void jce_model_draw_morphed_shadow(const JceModel *model,
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
JCE_API void jce_model_draw_velocity(const JceModel *model,
                             const JceRenderer *r, uint16_t view_id,
                             const JceModelVelocityCtx *ctx,
                             const jce_mat4 *transform,
                             const jce_mat4 *joint_matrices,
                             uint32_t num_joints,
                             const jce_mat4 *prev_transform,
                             const jce_mat4 *prev_joint_matrices,
                             uint32_t num_prev_joints);

/* ANIMATED-crowd velocity: per-limb motion vectors + world normals for `count`
 * uniquely-posed skinned instances in one instanced MRT submit per skinned
 * primitive, dual-skinning from THIS frame's bone texture (sampler stage 4)
 * and LAST frame's (stage 5, packed at the same bases).  Per-instance stream =
 * world (i_data0..3) + palette base (i_data4.x).  prog must be
 * vs_gbuffer_vel_skinned_inst + fs_gbuffer_vel; `roughness` fills the SSR
 * normal-buffer alpha (u_gbuffer_mat) for the whole batch. */
JCE_API void jce_model_draw_crowd_velocity_instanced(const JceModel *model,
                                                     const JceRenderer *r,
                                                     uint16_t view_id,
                                                     uint16_t prog_idx,
                                                     const JceModelVelocityCtx *ctx,
                                                     const jce_mat4 *worlds,
                                                     const uint32_t *bases,
                                                     uint32_t count,
                                                     uint16_t s_bones_idx,
                                                     uint16_t bone_tex_idx,
                                                     uint16_t s_prev_bones_idx,
                                                     uint16_t bone_prev_tex_idx,
                                                     uint16_t u_params_idx,
                                                     float tex_w, float tex_h,
                                                     uint16_t u_gbuffer_mat_idx,
                                                     float roughness);

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

/* Approximate resident GPU footprint of the model in bytes: the sum of every
 * primitive's vertex + index buffer bytes (estimated from vertex/index counts
 * and a representative PBR stride) plus the byte size of each bound material
 * texture (width*height*4, the registry-reported resident size).  Used by the
 * streaming budget so a chunk's residency reflects real VRAM rather than a flat
 * per-entity estimate (large-world VRAM ceiling).  Returns 0 for NULL.  This is
 * a scale-correct estimate, not an exact GPU allocation query (block-compressed
 * textures and exact vertex strides are approximated conservatively). */
JCE_API uint64_t      jce_model_gpu_bytes(const JceModel *model);

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

/* Imported morph-weight ANIMATION tracks (the glTF "weights" channel).
 *
 * Public for the same reason as the accessors above, and for one more: the
 * scene renderer drives blendshape animation from these, and it can only
 * reach engine/src/renderer/jce_model.h from inside that directory.  It was
 * therefore calling them through an implicit declaration, which returns int
 * -- so the track pointer came back with its top 32 bits gone. */
JCE_API uint32_t jce_model_morph_anim_count(const JceModel *model);

/* One track plus the animation / node it drives (out args may be NULL).
 * NULL when index is out of range.  Sample with
 * jce_morph_weight_track_sample to drive a node's per-instance weights. */
JCE_API const JceMorphWeightTrack *jce_model_morph_anim_track(
    const JceModel *model, uint32_t index,
    uint32_t *out_anim_index, uint32_t *out_node_index);

JCE_EXTERN_C_END

#endif /* JCE_MODEL_PUBLIC_H */
