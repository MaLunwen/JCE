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

/* Hi-Z occlusion (large-world #5, opt-in). Set the per-frame inputs BEFORE
 * jce_gpu_scene_dispatch: `depth_tex` = the depth-prepass texture idx (the cull
 * runs before this frame's prepass, so it is LAST frame's depth), `prev_vp` =
 * the view-proj that produced it, `view_w/h` = the color viewport size. When
 * enable is false (or Hi-Z unsupported) the cull stays frustum-only and this is
 * a no-op. UINT16_MAX depth_tex disables. */
JCE_API void jce_gpu_scene_set_hiz(JceGpuScene *gs, uint16_t depth_tex,
                                   const jce_mat4 *prev_vp, uint16_t view_w,
                                   uint16_t view_h, bool enable);

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

/* ── Foliage GPU-cull (千万 S2) ───────────────────────────────────────────
 * GPU frustum-cull a foliage scatter's PERSISTENT roots VB (the S1 instance
 * buffer: 4 vec4 = mat4 per instance) into a compacted visible buffer + a single
 * drawIndexedIndirect arg, so the GPU rasterises only the in-frustum subset.
 * Unlike jce_gpu_scene_dispatch (which reads the per-frame transient scene
 * records), this reads the persistent roots buffer DIRECTLY and computes each
 * instance's world-AABB on the GPU from the shared mesh's local AABB — no
 * per-frame CPU work, no per-instance AABB storage.  The visible/counter/indirect
 * buffers are OWNED by the caller (the foliage cache, one set per scatter) and
 * passed in by handle index.  Dispatches reset (reset_view) → cull/compact →
 * build-indirect (both cull_view).  Returns false when foliage cull is
 * unavailable (caps/programs) or inputs are degenerate. */
JCE_API bool jce_gpu_scene_foliage_supported(const JceGpuScene *gs);
JCE_API bool jce_gpu_scene_foliage_dispatch(JceGpuScene *gs,
                                            uint16_t reset_view, uint16_t cull_view,
                                            uint16_t roots_vb, uint16_t visible_vb,
                                            uint16_t counter_vb, uint16_t indirect_buf,
                                            uint32_t inst_count, uint32_t capacity,
                                            const jce_vec4 planes[6],
                                            jce_vec3 local_center,
                                            jce_vec3 local_extent,
                                            uint32_t num_indices);

/* 千万 S4 LOD-in-cull: run `band_count` distance-band cull passes over the same
 * roots VB + Hi-Z pyramid, each keeping only instances whose camera distance is
 * in [band_dmin[b], band_dmax[b]) and writing its own survivor/counter/indirect,
 * so the caller issues one drawIndexedIndirect per band at that band's reduced
 * LOD index buffer.  Arrays are indexed [0, band_count).  cam_pos feeds the
 * per-instance distance.  Returns false on unsupported / bad handles. */
/* 千万 S4/S5 banded LOD-in-cull, tile-aware.  begin resets the shared band
 * counters (cross-view UAV barrier) + builds the Hi-Z pyramid once; each tile
 * call culls + LOD-classifies one roots VB into the SHARED band partitions of
 * `visible_vb` (atomics make cross-tile appends safe); end writes one
 * drawIndexedIndirect element per band (startInstance = band * cap_band).
 * A non-tiled scatter is begin + one tile + end.  fade_w > 0 enables the
 * band-boundary cross-fade dual-write (千万 ②); far_dist > 0 culls beyond the
 * draw distance (fading out over fade_w). */
JCE_API bool jce_gpu_scene_foliage_lod_begin(JceGpuScene *gs, uint16_t reset_view,
                                             uint16_t counter_vb,
                                             uint32_t band_count,
                                             bool *out_hiz_ready);
JCE_API void jce_gpu_scene_foliage_lod_tile(JceGpuScene *gs, uint16_t cull_view,
                                            uint16_t roots_vb, uint32_t inst_count,
                                            uint16_t visible_vb, uint16_t counter_vb,
                                            const jce_vec4 planes[6],
                                            jce_vec3 local_center,
                                            jce_vec3 local_extent,
                                            jce_vec3 cam_pos, float far_dist,
                                            float band_step, uint32_t band_count,
                                            uint32_t cap_band, float fade_w,
                                            bool hiz_ready);
/* Nanite-lite V2: per-meshlet cluster cull for ONE hero mesh instance — one
 * thread per meshlet transforms its bounding sphere/cone by the entity world
 * matrix, frustum + cone-backface tests it, and writes drawIndexedIndirect
 * element m (culled → zero-index degenerate).  No counters/atomics/reset; the
 * caller then issues one bgfx_submit_indirect(view, prog, buf, 0, count) with
 * the meshlet-grouped index buffer bound.  meshlet_vb = the static
 * COMPUTE_READ records buffer from jce_skinned_mesh_set_meshlets (raw idx). */
JCE_API bool jce_gpu_scene_meshlet_supported(const JceGpuScene *gs);
/* V3.1: build the Hi-Z pyramid for this frame's meshlet cull dispatches
 * (call once per color pass, before the first dispatch).  Returns readiness;
 * pass it to every jce_gpu_scene_meshlet_dispatch as hiz_ready.
 * JCE_MESHLET_HIZ=0 disables (meshlet path only; foliage unaffected). */
JCE_API bool jce_gpu_scene_meshlet_hiz_prepare(JceGpuScene *gs, uint16_t view);
JCE_API void jce_gpu_scene_meshlet_dispatch(JceGpuScene *gs, uint16_t cull_view,
                                            uint16_t meshlet_vb, uint32_t count,
                                            float max_scale,
                                            const jce_mat4 *world,
                                            const jce_vec4 planes[6],
                                            jce_vec3 cam_pos,
                                            /* V3 DAG cut: world error per
                                             * unit distance; <= 0 = leaves */
                                            float err_k,
                                            /* V3.1: from meshlet_hiz_prepare
                                             * (false = frustum/cone only)  */
                                            bool hiz_ready,
                                            /* V4: shadow-cascade cull — keep
                                             * frustum, drop cone + Hi-Z (a
                                             * light-backfacing cluster still
                                             * casts).  planes = light VP.   */
                                            bool shadow_mode,
                                            uint16_t indirect_buf);

JCE_API bool jce_gpu_scene_foliage_lod_end(JceGpuScene *gs, uint16_t cull_view,
                                           uint16_t counter_vb,
                                           uint16_t indirect_buf,
                                           uint32_t band_count, uint32_t cap_band,
                                           const uint32_t *band_num_indices);

JCE_EXTERN_C_END

#endif /* JCE_GPU_SCENE_H */
