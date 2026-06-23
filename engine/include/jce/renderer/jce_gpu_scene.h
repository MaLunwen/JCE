/*
 * jce_gpu_scene.h -- GPU-driven rendering: persistent GPU instance buffer
 * ("GPUScene") + compute frustum cull (roadmap #18, Phase 0+1).
 *
 * Phase 0 (persistent buffer): one bgfx dynamic vertex buffer holds one
 * record per resident instance — { float4x4 world; float4 center+radius;
 * float4 extent; uint4 ids } (7 vec4 = 112 B).  It replaces the per-frame
 * transient instance-data buffer (the IDB memcpy in jce_model_draw_instanced)
 * with a GPU-resident store.
 *
 * Phase 1 (compute cull): cs_cull_frustum dispatches one thread per record,
 * tests its AABB against the 6 camera frustum planes, and atomically appends
 * the COMPACTED per-instance world matrix of survivors to a visible-instance
 * buffer.  The color draw then sources its instance data from that buffer
 * instead of a CPU-culled, CPU-packed transient buffer.
 *
 * Scope: the OPAQUE COLOR pass only.  Shadow / depth-prepass / velocity stay
 * on the CPU instancing path for now.  Flag-gated by the scene renderer
 * (JceSceneRenderConfig.gpu_driven / r.gpu_driven, default off): when off the
 * renderer never touches this module and the CPU path is byte-identical.
 *
 * Backend: bgfx compute (D3D11/D3D12/Vulkan/Metal/GL4/GLES3.1).  Guarded on
 * BGFX_CAPS_COMPUTE; jce_gpu_scene_is_supported() returns false on devices
 * without compute (or when the cull shader failed to load), and the caller
 * falls back to the CPU path.
 *
 * Layer: Render (Layer 4).
 */

#ifndef JCE_GPU_SCENE_H
#define JCE_GPU_SCENE_H

#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceGpuScene    JceGpuScene;
typedef struct JcePakArchive  JcePakArchive;

/* One resident-instance record fed to the GPUScene buffer.  Mirrors the 7-vec4
 * (112 B) layout consumed by cs_cull_frustum.sc.  The buffer is float-typed, so
 * the integer ids are stored as float VALUES (the shader does uint(ids.x)).
 * run_base / run_index are filled by jce_gpu_scene_add_run (the caller leaves
 * them as-is); they partition the shared visible buffer per model-run. */
typedef struct {
    jce_mat4 world;          /* 4 vec4 — model-to-world (column vectors)   */
    float    center[3];      /* world-space AABB center                    */
    float    radius;         /* bounding-sphere radius (unused by cull v1) */
    float    extent[3];      /* world-space AABB half-extents              */
    float    _pad_extent;
    float    run_base;       /* ids.x — slot offset of this run's partition */
    float    run_index;      /* ids.y — counter slot for this run           */
    float    _id_z;          /* ids.z — reserved (mesh, Phase 2)            */
    float    _id_w;          /* ids.w — reserved (material, Phase 2)        */
} JceGpuSceneRecord;

/* Create the GPU-driven scene helper.  Loads cs_cull_frustum from the engine
 * shader pak (with the embedded-engine-pak fallback).  Never returns NULL on a
 * compute-capable device unless allocation fails; on a non-compute device it
 * returns a valid handle in no-op mode (is_supported() == false). */
JCE_API JceGpuScene *jce_gpu_scene_create(const JcePakArchive *pak,
                                          jce_allocator_t alloc);

JCE_API void jce_gpu_scene_destroy(JceGpuScene *gs);

/* True iff compute is available AND the cull program loaded — i.e. the GPU
 * path can actually run.  The scene renderer ORs this with its gpu_driven
 * gate; false → fall back to the CPU instancing path. */
JCE_API bool jce_gpu_scene_is_supported(const JceGpuScene *gs);

/* ── Per-frame batch (one cull dispatch for the whole color-pass batch) ───
 *
 * Correctness note: bgfx orders the cull (compute view) entirely before the
 * color view, so ALL surviving instances of ALL model-runs must be resident in
 * the (single) visible buffer before the first draw.  The visible buffer is
 * therefore PARTITIONED per run: each run owns a contiguous slice [run_base,
 * run_base+count) into which its survivors compact, and the draw sources that
 * slice.  Usage per frame:
 *
 *   jce_gpu_scene_begin(gs);
 *   for each model-run:
 *       jce_gpu_scene_add_run(gs, records, count, &run_base);  // appends
 *   jce_gpu_scene_dispatch(gs, cull_view, planes);             // one dispatch
 *   for each model-run:
 *       // bind VB/IB/material, then per primitive:
 *       bgfx_set_instance_data_from_dynamic_vertex_buffer(
 *           jce_gpu_scene_visible_vb(gs), run_base, count);
 *       bgfx_submit(color_view, prog, ...);
 */

/* Begin a new per-frame batch (clears the appended run records). */
JCE_API void jce_gpu_scene_begin(JceGpuScene *gs);

/* Append one model-run's records to the batch.  Tags each record with its
 * partition base + run index (so the cull compacts within the partition) and
 * writes the partition base slot to *out_run_base.  Returns false (nothing
 * appended) when unsupported, count==0, or capacity is exhausted — the caller
 * then draws this run via the CPU path. */
JCE_API bool jce_gpu_scene_add_run(JceGpuScene *gs,
                                   const JceGpuSceneRecord *records,
                                   uint32_t count,
                                   uint32_t *out_run_base);

/* Upload all appended records, zero-clear the visible buffer + per-run
 * counters, and dispatch ONE compute frustum cull over the whole batch.
 *   cull_view : dedicated compute view (ordered before the color view).
 *   planes    : 6 normalised frustum planes (xyz=normal, w=d).
 * No-op (returns false) when unsupported or the batch is empty. */
JCE_API bool jce_gpu_scene_dispatch(JceGpuScene *gs, uint16_t cull_view,
                                    const jce_vec4 planes[6]);

/* The bgfx dynamic_vertex_buffer handle index of the compute-written visible
 * instance stream (mat4 per slot; byte-compatible with vs_pbr_inst i_data0..3).
 * UINT16_MAX before the first dispatch / when unsupported.  Pass to
 * bgfx_set_instance_data_from_dynamic_vertex_buffer(handle, run_base, count). */
JCE_API uint16_t jce_gpu_scene_visible_vb(const JceGpuScene *gs);

JCE_EXTERN_C_END

#endif /* JCE_GPU_SCENE_H */
