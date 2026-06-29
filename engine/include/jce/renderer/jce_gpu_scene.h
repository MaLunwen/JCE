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
 * Phase 2 (true indirect, roadmap #18 Direction C): when the GPU exposes
 * BGFX_CAPS_DRAW_INDIRECT, the cull does real STREAM COMPACTION + INDIRECT DRAW
 * instead of the 1:1-with-degenerate-slots fallback.  Three compute passes run
 * on the cull view: (1) cs_cull_reset zeroes the per-run survivor counters; (2)
 * cs_cull_compact tests each record and atomically appends survivors densely
 * into their run's partition of the visible buffer (culled records write
 * nothing); (3) cs_build_indirect reads each run's survivor counter + mesh index
 * count and writes that run's drawIndexedIndirect args (numInstances = survivor
 * count).  The renderer then issues ONE bgfx_submit_indirect per run, so there
 * is no CPU per-run fixed-count submit and no degenerate zero-area instances.
 * jce_gpu_scene_indirect_buffer() returns the filled indirect handle (or
 * UINT16_MAX when indirect is unavailable, in which case the caller uses the 1:1
 * fixed-count fallback path).
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

/* Returned by jce_gpu_scene_add_run: how the queued run is to be drawn.
 *   run_base    : the run's partition base slot in the visible buffer.  In the
 *                 INDIRECT path this is the startInstance the GPU writes into the
 *                 draw args; in the FALLBACK 1:1 path it is the fixed draw start.
 *   indirect_el : the run's element index in the indirect buffer (INDIRECT path
 *                 only; pass to bgfx_submit_indirect's _start).  UINT32_MAX when
 *                 indirect is unavailable -> caller uses the 1:1 fixed-count draw
 *                 with [run_base, run_base+count). */
typedef struct {
    uint32_t run_base;
    uint32_t indirect_el;
} JceGpuSceneRun;

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
 *       jce_gpu_scene_add_run(gs, records, count, num_indices, &run); // appends
 *   jce_gpu_scene_dispatch(gs, reset_view, cull_view, planes);        // dispatch
 *   for each model-run:
 *       if (jce_gpu_scene_is_indirect(gs)) {
 *           // bind VB/IB/material + instance source from slot 0, then:
 *           bgfx_submit_indirect(color_view, prog,
 *               jce_gpu_scene_indirect_buffer(gs), run.indirect_el, 1, ...);
 *       } else {
 *           // 1:1 fallback: fixed-count draw over the run's partition
 *           bgfx_set_instance_data_from_dynamic_vertex_buffer(
 *               jce_gpu_scene_visible_vb(gs), run.run_base, count);
 *           bgfx_submit(color_view, prog, ...);
 *       }
 */

/* Begin a new per-frame batch (clears the appended run records). */
JCE_API void jce_gpu_scene_begin(JceGpuScene *gs);

/* Append one model-run's records to the batch.  Tags each record with its
 * partition base + run index (so the cull compacts within the partition) and
 * reports how to draw the run in *out_run (partition base + indirect element).
 * `num_indices` is the run's mesh index count, recorded so the GPU can write the
 * run's drawIndexedIndirect args (ignored in the 1:1 fallback path).  Returns
 * false (nothing appended) when unsupported, count==0, or capacity is exhausted
 * — the caller then draws this run via the CPU path. */
JCE_API bool jce_gpu_scene_add_run(JceGpuScene *gs,
                                   const JceGpuSceneRecord *records,
                                   uint32_t count,
                                   uint32_t num_indices,
                                   JceGpuSceneRun *out_run);

/* Upload all appended records and dispatch the compute cull over the whole
 * batch.  In the indirect path this is THREE passes: counter reset (on
 * reset_view), cull+compact, build-indirect (both on cull_view); in the 1:1
 * fallback it is a single cull on cull_view.
 *   reset_view : a SEPARATE earlier compute view for the counter-reset pass —
 *                MUST be ordered before cull_view and distinct from it, so bgfx
 *                inserts a cross-view compute barrier between reset and compact
 *                (required on D3D12, where two dispatches on the SAME view keep
 *                the counter in UAV state and get NO barrier -> reset/atomic
 *                race).  Unused by the 1:1 fallback.
 *   cull_view  : dedicated compute view (ordered before the color view) for the
 *                cull/compact + build-indirect passes.
 *   planes     : 6 normalised frustum planes (xyz=normal, w=d).
 * No-op (returns false) when unsupported or the batch is empty. */
JCE_API bool jce_gpu_scene_dispatch(JceGpuScene *gs, uint16_t reset_view,
                                    uint16_t cull_view, const jce_vec4 planes[6]);

/* The bgfx dynamic_vertex_buffer handle index of the compute-written visible
 * instance stream (mat4 per slot; byte-compatible with vs_pbr_inst i_data0..3).
 * UINT16_MAX before the first dispatch / when unsupported.  Pass to
 * bgfx_set_instance_data_from_dynamic_vertex_buffer(handle, run_base, count). */
JCE_API uint16_t jce_gpu_scene_visible_vb(const JceGpuScene *gs);

/* True iff the last create resolved the INDIRECT path (BGFX_CAPS_DRAW_INDIRECT +
 * the compact/indirect compute programs loaded).  When true, a successful
 * dispatch fills the indirect buffer and the caller should issue one
 * bgfx_submit_indirect per run using JceGpuSceneRun.indirect_el; when false the
 * caller falls back to the 1:1 fixed-count draw from the visible buffer. */
JCE_API bool jce_gpu_scene_is_indirect(const JceGpuScene *gs);

/* The bgfx indirect_buffer handle index holding the per-run drawIndexedIndirect
 * args (one element per queued run), filled by jce_gpu_scene_dispatch.  Pass to
 * bgfx_submit_indirect(view, prog, handle, run.indirect_el, 1, ...).  UINT16_MAX
 * when the indirect path is unavailable / before the first dispatch. */
JCE_API uint16_t jce_gpu_scene_indirect_buffer(const JceGpuScene *gs);

JCE_EXTERN_C_END

#endif /* JCE_GPU_SCENE_H */
