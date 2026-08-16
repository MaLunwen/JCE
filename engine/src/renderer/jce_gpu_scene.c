/*
 * jce_gpu_scene.c -- GPU-driven rendering: compute frustum cull (roadmap #18).
 * See jce_gpu_scene.h.
 *
 * TWO CULL PATHS
 * --------------------------------------------------------------------------
 * INDIRECT (preferred, roadmap #18 Direction C): when the GPU exposes
 *   BGFX_CAPS_DRAW_INDIRECT and the compact/indirect compute programs load, the
 *   cull does TRUE STREAM COMPACTION + INDIRECT DRAW.  Three compute passes on
 *   the cull view:
 *     1. cs_cull_reset    : zero the per-group survivor counters (GPU clear; no
 *                           CPU bgfx_update_dynamic_* -> avoids the D3D12 staging
 *                           NULL-deref under pressure).
 *     2. cs_cull_compact  : one thread per record; survivors atomicAdd into their
 *                           group counter and write their mat4 DENSELY into the
 *                           group's partition of the visible buffer. Culled
 *                           records write nothing.
 *     3. cs_build_indirect: one thread per primitive draw; reads its owning
 *                           group's survivor counter and writes that primitive's
 *                           drawIndexedIndirect args.
 *   Multi-primitive models therefore pay one record upload and cull per instance,
 *   while retaining one indirect submit per primitive.
 *
 * FALLBACK 1:1 (no BGFX_CAPS_DRAW_INDIRECT): the redesigned cs_cull_frustum
 *   writes EVERY visible-buffer slot 1:1 (record id -> slot id); survivors get
 *   their world matrix and culled records get a ZERO matrix (degenerate
 *   instance).  No atomic counter, no indirect buffer; the caller draws each run
 *   with a CPU fixed-count submit over [run_base, run_base+count).
 *
 * STAGING-FREE DESIGN (no per-frame bgfx_update_dynamic_* on compute buffers)
 * --------------------------------------------------------------------------
 * Scene records and draw metadata use persistent COMPUTE_READ dynamic buffers,
 * because transient VBs do not expose shader-resource views on D3D11/D3D12.
 * Visible/counter buffers are written only by compute and cleared by the GPU
 * reset pass. jce_gpu_scene_dispatch updates each read-only input buffer once.
 *
 * One per-frame BATCH covers the whole color-pass instanced set; bgfx orders the
 * compute view ahead of the color view and inserts UAV barriers between the three
 * same-view compute dispatches, so the indirect args + compact instances are
 * resident before the first draw.
 *
 * Layout (must mirror the .sc shaders):
 *   scene_buf  (dynamic VB)   : 7 vec4 per record (stride 112 B), bound RO;
 *                               persistent and updated once per dispatch.
 *   visible_buf (dynamic VB)  : 4 vec4 per slot (a mat4 instance stream),
 *                               COMPUTE_READ_WRITE; compact per group (indirect) or
 *                               1:1 slot==record id (fallback).
 *   counter_buf (dynamic VB)  : 1 uint per visibility group (indirect only)
 *   runmeta_buf (dynamic VB)  : 1 uvec4 per primitive draw
 *                               (numIndices, run_base, counter_index, _)
 *   indirect_buf (indirect)   : 1 element per primitive draw
 */

#include <jce/renderer/jce_gpu_scene.h>
#include <jce/renderer/jce_shaders.h>   /* embedded engine pak fallback */
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_shader_load.h"   /* shared backend suffix */

#include <bgfx/c99/bgfx.h>

#include <stdio.h>
#include <stdlib.h>   /* getenv (diag toggles) */
#include <string.h>

#define LOG_TAG "gpu-scene"

#define CULL_THREADS_X      64u
#define SCENE_VEC4_PER_REC  7u
#define SCENE_STRIDE_BYTES  (SCENE_VEC4_PER_REC * 16u)   /* 112 B */
#define VIS_VEC4_PER_SLOT   4u
#define VIS_STRIDE_BYTES    (VIS_VEC4_PER_SLOT * 16u)    /* 64 B (mat4) */
#define RUNMETA_STRIDE_BYTES 16u                         /* one uvec4 per primitive draw */
#define MAX_SCENE_RECORDS    (UINT32_MAX / SCENE_STRIDE_BYTES)
#define MAX_VISIBLE_SLOTS    (UINT32_MAX / VIS_STRIDE_BYTES)
#define MAX_DRAW_METADATA    (UINT32_MAX / RUNMETA_STRIDE_BYTES)

/* The record struct MUST be exactly the 7-vec4 (112 B) GPU layout so a straight
 * memcpy populates the scene buffer.  C99 has no _Static_assert; use the
 * negative-array-size compile-time check idiom. */
typedef char jce_gpu_scene_record_size_check[
    (sizeof(JceGpuSceneRecord) == SCENE_STRIDE_BYTES) ? 1 : -1];

struct JceGpuScene {
    bool            supported;
    bool            indirect;        /* indirect compaction path resolved at create */
    bool            indirect_ready;  /* cs_build_indirect actually ran THIS dispatch:
                                      * the indirect buffer holds VALID args only when
                                      * true.  Reset every dispatch/begin; set only
                                      * after pass-3.  Guards the draw side from
                                      * consuming stale/uninitialised indirect args
                                      * when a per-frame prerequisite fails and the
                                      * dispatch falls through to the 1:1 cull. */
    jce_allocator_t alloc;

    /* Fallback 1:1 cull program. */
    bgfx_program_handle_t cull_program;
    /* Indirect compaction programs. */
    bgfx_program_handle_t reset_program;
    bgfx_program_handle_t compact_program;
    bgfx_program_handle_t build_program;

    /* Persistent visible-instance buffer (grown on demand to the largest batch
     * seen).  Written by the cull; never CPU-updated. */
    bgfx_dynamic_vertex_buffer_handle_t visible_buf;
    uint32_t        capacity;        /* visible slots the buffer can hold */

    /* Persistent survivor counter buffer (indirect path). One uint per
     * visibility group; zeroed by reset and incremented by compact. */
    bgfx_dynamic_vertex_buffer_handle_t counter_buf;
    uint32_t        counter_cap;     /* visibility groups it can hold */

    /* Indirect draw-args buffer (indirect path). One element per primitive. */
    bgfx_indirect_buffer_handle_t indirect_buf;
    uint32_t        indirect_cap;    /* primitive draws it can hold */

    /* Persistent COMPUTE-READ scene-record buffer.  A bgfx TRANSIENT VB is
     * created with flags=NONE, so on D3D11/D3D12 it has NO shader-resource view;
     * binding it as the compute cull's `b_scene` SRV reads all-ZERO records →
     * every instance's world matrix is zero → degenerate (zero-area) instances →
     * empty screen.  GL/Vulkan bind any VBO as an SSBO so they masked this, which
     * is why the GL-validated GPU-driven campaign never caught it.  The records
     * therefore live in a persistent dynamic VB created COMPUTE_READ + updated
     * each frame; its SRV starts at element 0 (no ring rebase → scene_off = 0). */
    bgfx_dynamic_vertex_buffer_handle_t scene_buf;
    uint32_t        scene_cap;       /* records the scene buffer can hold */
    /* Persistent COMPUTE-READ draw-meta buffer (indirect path) — same transient-VB
     * NULL-SRV defect as scene_buf; one uvec4 per primitive draw. */
    bgfx_dynamic_vertex_buffer_handle_t runmeta_gpu_buf;
    uint32_t        runmeta_gpu_cap;

    bgfx_vertex_layout_t scene_layout;     /* 7 vec4 (persistent scene records) */
    bgfx_vertex_layout_t visible_layout;   /* 4 vec4 (instance mat4)           */
    bgfx_vertex_layout_t counter_layout;   /* 1 uint per visibility group      */
    bgfx_vertex_layout_t runmeta_layout;   /* 1 uvec4 per primitive draw       */

    bgfx_uniform_handle_t u_cull_planes;        /* vec4[6] */
    bgfx_uniform_handle_t u_cull_params;        /* vec4    */
    bgfx_uniform_handle_t u_cull_reset_params;  /* vec4    */
    bgfx_uniform_handle_t u_indirect_params;    /* vec4    */

    /* Foliage GPU-cull (千万 S2): culls S1's persistent roots VB directly —
     * computes per-instance world-AABB on the GPU from the shared mesh local
     * AABB, compacts survivors, builds ONE indirect draw.  The visible/counter/
     * indirect buffers are owned per-scatter by the foliage cache (passed in);
     * this only holds the programs + uniforms.  Reuses reset_program +
     * u_cull_reset_params (run_count=1) and u_cull_planes. */
    bgfx_program_handle_t meshlet_cull_program;   /* Nanite-lite V2 (cs_meshlet_cull) */
    bool                  meshlet_supported;
    bgfx_uniform_handle_t u_mlcull_params;        /* vec4 (count, max_scale, force, 0) */
    bgfx_uniform_handle_t u_mlcull_model;         /* mat4 entity world matrix */
    bgfx_program_handle_t foliage_cull_program;
    bgfx_program_handle_t foliage_indirect_program;
    bgfx_uniform_handle_t u_fcull_params;   /* vec4    (count, capacity, _, hiz) */
    bgfx_uniform_handle_t u_fcull_aabb;     /* vec4[2] (local center, extent)   */
    bgfx_uniform_handle_t u_find_params;
    bgfx_uniform_handle_t u_find_counts;    /* vec4[2] — per-band LOD index counts (千万 S4) */    /* vec4    (num_indices, start,_,_)  */
    bgfx_uniform_handle_t u_fcull_lod;      /* vec4    (dist_min, dist_max,_,_)  — 千万 S4 */
    bgfx_uniform_handle_t u_cull_campos;    /* vec4    (cam.xyz, 0) for the band distance */
    bool                  foliage_supported;

    /* Hi-Z occlusion (large-world #5, opt-in). A single-level MAX-depth image is
     * built from the depth prepass each frame (cs_hiz_build, one dispatch on the
     * pre-cull reset_view) and the cull tests each instance's screen AABB against
     * it. Per-frame inputs are set by jce_gpu_scene_set_hiz BEFORE dispatch. The
     * cull runs before this frame's prepass, so the depth + VP are LAST frame's
     * (1-frame-late Hi-Z; conservative). Off → params.w stays 0 (frustum-only). */
    bgfx_program_handle_t hiz_program;      /* cs_hiz_build  (mip0 from depth)   */
    bgfx_program_handle_t hiz_reduce_program; /* cs_hiz_reduce (mip i from mip i-1) */
    bgfx_uniform_handle_t u_hiz_build;      /* vec4 {dst_w,dst_h,src_lod,0}     */
    bgfx_uniform_handle_t s_hiz;            /* sampler: pyramid, read by the cull */
    bgfx_uniform_handle_t s_hiz_src;        /* sampler: depth src, read by build  */
    bgfx_uniform_handle_t u_cull_viewproj;  /* mat4: the VP that made the pyramid */
    bgfx_uniform_handle_t u_hiz_params;     /* vec4 {w,h,num_mips,0}             */
    bgfx_texture_handle_t hiz_tex;          /* mipped r32f MAX-depth pyramid     */
    uint16_t              hiz_w, hiz_h;
    uint8_t               hiz_mips;         /* mip levels in hiz_tex (>=1)       */
    bool                  hiz_supported;
    /* per-frame (set_hiz) */
    bgfx_texture_handle_t hiz_src_depth;
    jce_mat4              hiz_prev_vp;
    uint16_t              hiz_view_w, hiz_view_h;
    bool                  hiz_on;

    /* Per-frame host batch accumulated by add_draw_group and uploaded once. */
    JceGpuSceneRecord *rec;
    uint32_t           rec_count;
    uint32_t           rec_cap;

    /* Per-draw metadata. Several primitive draws may share one visibility
     * group and therefore the same run_base + survivor counter. */
    struct {
        uint32_t num_indices;
        uint32_t run_base;
        uint32_t counter_index;
    } *runmeta;
    uint32_t           group_count;  /* visibility groups this frame */
    uint32_t           run_count;    /* primitive draws this frame */
    uint32_t           runmeta_cap;
    JceGpuSceneFrameStats frame_stats;
};

/* ── shader loading ───────────────────────────────────────────────────── *
 * The backend suffix comes from renderer/jce_shader_load.h; the loader stays
 * local because of the JCE_SHADER_DIAG instrumentation below. */

static bgfx_shader_handle_t load_shader(const JcePakArchive *pak,
                                        const char *name, const char *sfx)
{
    bgfx_shader_handle_t invalid = { UINT16_MAX };
    char path[256];
    snprintf(path, sizeof(path), "shaders/%s_%s.bin", name, sfx);

    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fb = jce_shaders_embedded_engine_pak();
        if (fb && fb != pak) {
            asset = jce_pak_find(fb, path);
            if (asset) pak = fb;
        }
    }
    if (!asset) {
        LOG_ERROR(LOG_TAG, "shader not found in pak: %s", path);
        return invalid;
    }
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return invalid;
    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) { JCE_FREE(buf); return invalid; }
    /* DIAG (JCE_SHADER_DIAG): log which pak served the blob + its GLSL version
     * line — pinpoints stale-blob sources across the pak fallback chain. */
    if (getenv("JCE_SHADER_DIAG")) {
        const char *v = NULL;
        for (size_t i = 0; i + 12 < n; ++i)
            if (memcmp((char *)buf + i, "#version", 8) == 0) { v = (char *)buf + i; break; }
        char vline[32] = {0};
        if (v) { size_t k = 0; while (k < 31 && v[k] && v[k] != '\n') { vline[k] = v[k]; ++k; } }
        LOG_INFO(LOG_TAG, "shader %s: %u bytes, pak=%p, version='%s'",
                 path, (unsigned)n, (const void *)pak, vline[0] ? vline : "(none)");
    }
    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);
    return bgfx_create_shader(mem);
}

static bgfx_program_handle_t load_compute(const JcePakArchive *pak,
                                          const char *name, const char *sfx)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    bgfx_shader_handle_t cs = load_shader(pak, name, sfx);
    if (cs.idx == UINT16_MAX) return invalid;
    return bgfx_create_compute_program(cs, true);
}

/* ── buffer (re)allocation ────────────────────────────────────────────── */

static void destroy_visible_buffer(JceGpuScene *gs)
{
    if (gs->visible_buf.idx != UINT16_MAX) {
        bgfx_destroy_dynamic_vertex_buffer(gs->visible_buf);
        gs->visible_buf.idx = UINT16_MAX;
    }
    gs->capacity = 0;
}

/* Choose a geometric capacity without wrapping uint32_t. Alignment is a
 * performance preference, not a correctness requirement: near the API limit
 * the exact requested capacity is valid even when it cannot be rounded up. */
static bool choose_capacity(uint32_t current, uint32_t need, uint32_t initial,
                            uint32_t alignment, uint32_t max_count,
                            uint32_t *out_capacity)
{
    uint32_t cap;

    if (!out_capacity || need == 0u || need > max_count ||
        current > max_count || initial == 0u)
        return false;

    cap = current;
    if (cap < need) {
        if (cap < initial) cap = initial;
        if (cap > max_count) cap = need;

        while (cap < need) {
            if (cap > max_count / 2u) {
                cap = need;
                break;
            }
            cap *= 2u;
        }
    }

    if (alignment > 1u) {
        uint32_t remainder = cap % alignment;
        if (remainder != 0u) {
            uint32_t add = alignment - remainder;
            if (add <= max_count - cap) cap += add;
        }
    }

    if (cap < need || cap > max_count) return false;
    *out_capacity = cap;
    return true;
}

/* Ensure the visible buffer holds at least `need` slots.  Grows geometrically;
 * not shrunk (steady-state size stabilises).  This is a Default-heap resource
 * created once per growth (createCommittedResource at create time only — NOT a
 * per-frame staging allocation), so it never hits the per-call Upload-heap
 * staging path that crashed. */
static bool ensure_capacity(JceGpuScene *gs, uint32_t need)
{
    if (need <= gs->capacity && gs->visible_buf.idx != UINT16_MAX) return true;

    uint32_t cap;
    if (!choose_capacity(gs->capacity, need, 1024u, CULL_THREADS_X,
                         MAX_VISIBLE_SLOTS, &cap))
        return false;

    destroy_visible_buffer(gs);
    gs->frame_stats.buffer_growths++;

    gs->visible_buf = bgfx_create_dynamic_vertex_buffer(
        cap, &gs->visible_layout,
        BGFX_BUFFER_COMPUTE_READ_WRITE
        | BGFX_BUFFER_COMPUTE_FORMAT_32X4
        | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);

    if (gs->visible_buf.idx == UINT16_MAX) {
        destroy_visible_buffer(gs);
        return false;
    }
    gs->capacity = cap;
    return true;
}

/* Ensure the survivor counter buffer holds at least `groups` uints.
 * One uint per visibility group; written and cleared only by compute. */
static bool ensure_counter_capacity(JceGpuScene *gs, uint32_t groups)
{
    if (groups <= gs->counter_cap && gs->counter_buf.idx != UINT16_MAX) return true;

    uint32_t cap;
    if (!choose_capacity(gs->counter_cap, groups, 256u, CULL_THREADS_X,
                         UINT32_MAX / 4u, &cap))
        return false;

    if (gs->counter_buf.idx != UINT16_MAX) {
        bgfx_destroy_dynamic_vertex_buffer(gs->counter_buf);
        gs->counter_buf.idx = UINT16_MAX;
        gs->counter_cap = 0;
    }
    gs->frame_stats.buffer_growths++;
    gs->counter_buf = bgfx_create_dynamic_vertex_buffer(
        cap, &gs->counter_layout,
        BGFX_BUFFER_COMPUTE_READ_WRITE
        | BGFX_BUFFER_COMPUTE_FORMAT_32X1
        | BGFX_BUFFER_COMPUTE_TYPE_UINT);
    if (gs->counter_buf.idx == UINT16_MAX) return false;
    gs->counter_cap = cap;
    return true;
}

/* Ensure the persistent COMPUTE_READ scene-record buffer holds >= `need` records.
 * See the scene_buf field comment: a transient VB has no D3D11 SRV, so the cull's
 * b_scene read returns zeros — the records MUST live in a COMPUTE_READ buffer. */
static bool ensure_scene_capacity(JceGpuScene *gs, uint32_t need)
{
    if (need <= gs->scene_cap && gs->scene_buf.idx != UINT16_MAX) return true;

    uint32_t cap;
    if (!choose_capacity(gs->scene_cap, need, 1024u, 1u,
                         MAX_SCENE_RECORDS, &cap))
        return false;

    if (gs->scene_buf.idx != UINT16_MAX) {
        bgfx_destroy_dynamic_vertex_buffer(gs->scene_buf);
        gs->scene_buf.idx = UINT16_MAX;
        gs->scene_cap = 0;
    }
    gs->frame_stats.buffer_growths++;
    gs->scene_buf = bgfx_create_dynamic_vertex_buffer(
        cap, &gs->scene_layout,
        BGFX_BUFFER_COMPUTE_READ
        | BGFX_BUFFER_COMPUTE_FORMAT_32X4
        | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    if (gs->scene_buf.idx == UINT16_MAX) return false;
    gs->scene_cap = cap;
    return true;
}

/* Ensure the persistent COMPUTE_READ draw-metadata buffer holds at least
 * `draws` entries. Same transient-VB NULL-SRV fix as ensure_scene_capacity. */
static bool ensure_runmeta_gpu_capacity(JceGpuScene *gs, uint32_t draws)
{
    if (draws <= gs->runmeta_gpu_cap && gs->runmeta_gpu_buf.idx != UINT16_MAX) return true;

    uint32_t cap;
    if (!choose_capacity(gs->runmeta_gpu_cap, draws, 256u, 1u,
                         MAX_DRAW_METADATA, &cap))
        return false;

    if (gs->runmeta_gpu_buf.idx != UINT16_MAX) {
        bgfx_destroy_dynamic_vertex_buffer(gs->runmeta_gpu_buf);
        gs->runmeta_gpu_buf.idx = UINT16_MAX;
        gs->runmeta_gpu_cap = 0;
    }
    gs->frame_stats.buffer_growths++;
    gs->runmeta_gpu_buf = bgfx_create_dynamic_vertex_buffer(
        cap, &gs->runmeta_layout,
        BGFX_BUFFER_COMPUTE_READ
        | BGFX_BUFFER_COMPUTE_FORMAT_32X4
        | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);
    if (gs->runmeta_gpu_buf.idx == UINT16_MAX) return false;
    gs->runmeta_gpu_cap = cap;
    return true;
}

/* Ensure the indirect buffer holds at least `draws` primitive draw elements. */
static bool ensure_indirect_capacity(JceGpuScene *gs, uint32_t draws)
{
    if (draws <= gs->indirect_cap && gs->indirect_buf.idx != UINT16_MAX) return true;

    uint32_t cap;
    if (!choose_capacity(gs->indirect_cap, draws, 256u, 1u,
                         UINT32_MAX, &cap))
        return false;

    if (gs->indirect_buf.idx != UINT16_MAX) {
        bgfx_destroy_indirect_buffer(gs->indirect_buf);
        gs->indirect_buf.idx = UINT16_MAX;
        gs->indirect_cap = 0;
    }
    gs->frame_stats.buffer_growths++;
    gs->indirect_buf = bgfx_create_indirect_buffer(cap);
    if (gs->indirect_buf.idx == UINT16_MAX) return false;
    gs->indirect_cap = cap;
    return true;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

JceGpuScene *jce_gpu_scene_create(const JcePakArchive *pak, jce_allocator_t alloc)
{
    JceGpuScene *gs = (JceGpuScene *)alloc.alloc(sizeof(JceGpuScene), alloc.ctx);
    if (!gs) return NULL;
    memset(gs, 0, sizeof(*gs));
    gs->alloc = alloc;
    gs->cull_program.idx       = UINT16_MAX;
    gs->reset_program.idx      = UINT16_MAX;
    gs->compact_program.idx    = UINT16_MAX;
    gs->build_program.idx      = UINT16_MAX;
    gs->visible_buf.idx        = UINT16_MAX;
    gs->counter_buf.idx        = UINT16_MAX;
    gs->indirect_buf.idx       = UINT16_MAX;
    gs->scene_buf.idx          = UINT16_MAX;
    gs->runmeta_gpu_buf.idx    = UINT16_MAX;
    gs->u_cull_planes.idx      = UINT16_MAX;
    gs->u_cull_params.idx      = UINT16_MAX;
    gs->u_cull_reset_params.idx = UINT16_MAX;
    gs->u_indirect_params.idx  = UINT16_MAX;
    gs->foliage_cull_program.idx     = UINT16_MAX;
    gs->foliage_indirect_program.idx = UINT16_MAX;
    gs->meshlet_cull_program.idx     = UINT16_MAX;
    gs->u_mlcull_params.idx          = UINT16_MAX;
    gs->u_mlcull_model.idx           = UINT16_MAX;
    gs->u_fcull_params.idx = UINT16_MAX;
    gs->u_fcull_aabb.idx   = UINT16_MAX;
    gs->u_find_params.idx  = UINT16_MAX;
    gs->u_find_counts.idx  = UINT16_MAX;
    gs->u_fcull_lod.idx    = UINT16_MAX;
    gs->u_cull_campos.idx  = UINT16_MAX;
    gs->hiz_program.idx     = UINT16_MAX;
    gs->hiz_reduce_program.idx = UINT16_MAX;
    gs->u_hiz_build.idx     = UINT16_MAX;
    gs->s_hiz.idx           = UINT16_MAX;
    gs->s_hiz_src.idx       = UINT16_MAX;
    gs->u_cull_viewproj.idx = UINT16_MAX;
    gs->u_hiz_params.idx    = UINT16_MAX;
    gs->hiz_tex.idx         = UINT16_MAX;
    gs->hiz_src_depth.idx   = UINT16_MAX;

    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !(caps->supported & BGFX_CAPS_COMPUTE)) {
        /* Expected on GLES3/WebGL2-class backends; several gpu-scene
         * instances are created per app, so say it once, not per instance. */
        static bool warned_no_compute = false;
        if (!warned_no_compute) {
            warned_no_compute = true;
            LOG_WARN(LOG_TAG,
                     "GPU has no compute support; GPU-driven path disabled");
        }
        return gs;  /* no-op mode */
    }

    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) {
        LOG_WARN(LOG_TAG, "no shader suffix for current renderer; GPU-driven off");
        return gs;
    }

    /* GL ORDER CONTRACT: create EVERY uniform BEFORE the first program that
     * references it.  bgfx's GL backend resolves a program's user uniforms
     * against the uniform REGISTRY at program-create time (glGetActiveUniform
     * + name lookup); a uniform registered AFTER the program is created is
     * silently absent from that program's constant buffer, so bgfx never
     * uploads it — the GL shader then reads 0 (e.g. u_fcull_params.x count==0
     * → the foliage cull culled EVERYTHING on OpenGL while D3D11 was fine;
     * root-caused 2026-07-03 via RenderDoc shader-replacement probes).  D3D/
     * Vulkan resolve uniforms from the shader blob's own table instead, which
     * masked the ordering bug on those backends. */
    gs->u_cull_planes       = bgfx_create_uniform("u_cull_planes",       BGFX_UNIFORM_TYPE_VEC4, 6);
    gs->u_cull_params       = bgfx_create_uniform("u_cull_params",       BGFX_UNIFORM_TYPE_VEC4, 1);
    gs->u_cull_reset_params = bgfx_create_uniform("u_cull_reset_params", BGFX_UNIFORM_TYPE_VEC4, 1);
    gs->u_indirect_params   = bgfx_create_uniform("u_indirect_params",   BGFX_UNIFORM_TYPE_VEC4, 1);
    gs->u_fcull_params      = bgfx_create_uniform("u_fcull_params",      BGFX_UNIFORM_TYPE_VEC4, 1);
    gs->u_fcull_aabb        = bgfx_create_uniform("u_fcull_aabb",        BGFX_UNIFORM_TYPE_VEC4, 2);
    gs->u_find_params       = bgfx_create_uniform("u_find_params",       BGFX_UNIFORM_TYPE_VEC4, 1);
    gs->u_find_counts       = bgfx_create_uniform("u_find_counts",       BGFX_UNIFORM_TYPE_VEC4, 2);
    gs->u_fcull_lod         = bgfx_create_uniform("u_fcull_lod",         BGFX_UNIFORM_TYPE_VEC4, 1);
    gs->u_cull_campos       = bgfx_create_uniform("u_cull_campos",       BGFX_UNIFORM_TYPE_VEC4, 1);
    gs->u_hiz_build         = bgfx_create_uniform("u_hiz_build",         BGFX_UNIFORM_TYPE_VEC4,    1);
    gs->s_hiz               = bgfx_create_uniform("s_hiz",               BGFX_UNIFORM_TYPE_SAMPLER, 1);
    gs->s_hiz_src           = bgfx_create_uniform("s_src",               BGFX_UNIFORM_TYPE_SAMPLER, 1);
    gs->u_cull_viewproj     = bgfx_create_uniform("u_cull_viewproj",     BGFX_UNIFORM_TYPE_MAT4,    1);
    gs->u_hiz_params        = bgfx_create_uniform("u_hiz_params",        BGFX_UNIFORM_TYPE_VEC4,    1);
    gs->u_mlcull_params     = bgfx_create_uniform("u_mlcull_params",     BGFX_UNIFORM_TYPE_VEC4,    1);
    gs->u_mlcull_model      = bgfx_create_uniform("u_mlcull_model",      BGFX_UNIFORM_TYPE_MAT4,    1);

    /* Always load the 1:1 fallback cull program (used when indirect is absent). */
    gs->cull_program = load_compute(pak, "cs_cull_frustum", sfx);
    if (gs->cull_program.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "cs_cull_frustum load failed; GPU-driven path disabled");
        return gs;
    }

    /* scene_buf layout: 7 vec4 (TEXCOORD0..6). The persistent compute-readable
     * buffer holds tightly packed 112-byte records. */
    bgfx_vertex_layout_begin(&gs->scene_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&gs->scene_layout, BGFX_ATTRIB_TEXCOORD0, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->scene_layout, BGFX_ATTRIB_TEXCOORD1, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->scene_layout, BGFX_ATTRIB_TEXCOORD2, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->scene_layout, BGFX_ATTRIB_TEXCOORD3, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->scene_layout, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->scene_layout, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->scene_layout, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&gs->scene_layout);

    /* visible_buf layout: 4 vec4 instance matrix — MUST match the per-instance
     * stream vs_pbr_inst reads (i_data0..3 = TEXCOORD7..4), so this is byte-
     * compatible with the transient IDB the CPU path uses. */
    bgfx_vertex_layout_begin(&gs->visible_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&gs->visible_layout, BGFX_ATTRIB_TEXCOORD7, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->visible_layout, BGFX_ATTRIB_TEXCOORD6, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->visible_layout, BGFX_ATTRIB_TEXCOORD5, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&gs->visible_layout, BGFX_ATTRIB_TEXCOORD4, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&gs->visible_layout);

    /* counter_buf layout: one uint per visibility group. */
    bgfx_vertex_layout_begin(&gs->counter_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&gs->counter_layout, BGFX_ATTRIB_TEXCOORD0, 1, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&gs->counter_layout);

    /* runmeta_buf layout: one uvec4 per primitive draw (carried as 4 floats;
     * the shader converts the RGBA32F values to uints). */
    bgfx_vertex_layout_begin(&gs->runmeta_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&gs->runmeta_layout, BGFX_ATTRIB_TEXCOORD0, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&gs->runmeta_layout);

    /* (u_cull_planes / u_cull_params created ABOVE, before program loads —
     * GL order contract.) */

    /* Hi-Z occlusion resources (large-world #5): the pyramid-build compute
     * program + the cull-side sampler/uniforms. Needs R32F compute-image write.
     * The cull shaders declare s_hiz/u_cull_viewproj/u_hiz_params; the handles
     * were created above (GL order contract).  When Hi-Z is off the cull just
     * never sees u_cull_params.w>0.5 and the block is skipped. */
    if (caps->formats[BGFX_TEXTURE_FORMAT_R32F] & BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE) {
        gs->hiz_program = load_compute(pak, "cs_hiz_build", sfx);
        if (gs->hiz_program.idx != UINT16_MAX) {
            /* Full-pyramid reduce (cs_hiz_reduce): mip i from mip i-1.  Optional —
             * if it fails to load, hiz_build falls back to the single-level pyramid
             * (num_mips clamped to 1) and the cull only tests <=1-texel footprints. */
            gs->hiz_reduce_program = load_compute(pak, "cs_hiz_reduce", sfx);
            gs->hiz_supported = true;
            LOG_INFO(LOG_TAG, "Hi-Z occlusion resources ready (opt-in JCE_HIZ_OCCLUSION)%s",
                     gs->hiz_reduce_program.idx != UINT16_MAX ? " [full mip pyramid]" : " [single-level]");
        }
    }

    gs->supported = true;

    /* Resolve the INDIRECT path: needs BGFX_CAPS_DRAW_INDIRECT + the three
     * compaction/indirect compute programs.  If any piece is missing, stay on
     * the 1:1 fallback (still a valid GPU cull, just no compaction). */
    if (caps->supported & BGFX_CAPS_DRAW_INDIRECT) {
        gs->reset_program   = load_compute(pak, "cs_cull_reset",    sfx);
        gs->compact_program = load_compute(pak, "cs_cull_compact",  sfx);
        gs->build_program   = load_compute(pak, "cs_build_indirect", sfx);
        if (gs->reset_program.idx   != UINT16_MAX &&
            gs->compact_program.idx != UINT16_MAX &&
            gs->build_program.idx   != UINT16_MAX) {
            /* (uniforms created above, before the program loads.) */
            gs->indirect = true;

            /* Nanite-lite V2: per-meshlet cluster cull (needs only
             * DRAW_INDIRECT + compute; no counters/atomics). */
            gs->meshlet_cull_program = load_compute(pak, "cs_meshlet_cull", sfx);
            gs->meshlet_supported = (gs->meshlet_cull_program.idx != UINT16_MAX);
            if (!gs->meshlet_supported)
                LOG_WARN(LOG_TAG, "cs_meshlet_cull unavailable; meshlet cull disabled");

            /* Foliage GPU-cull programs (千万 S2): cull S1's persistent roots VB
             * directly + build one indirect draw.  Needs the same DRAW_INDIRECT
             * cap + reuses reset_program/u_cull_reset_params/u_cull_planes. */
            gs->foliage_cull_program     = load_compute(pak, "cs_foliage_cull",     sfx);
            gs->foliage_indirect_program = load_compute(pak, "cs_foliage_indirect", sfx);
            if (gs->foliage_cull_program.idx     != UINT16_MAX &&
                gs->foliage_indirect_program.idx != UINT16_MAX) {
                /* (u_fcull_* / u_find_params created above — GL order contract.) */
                gs->foliage_supported = true;
            } else {
                if (gs->foliage_cull_program.idx     != UINT16_MAX) { bgfx_destroy_program(gs->foliage_cull_program);     gs->foliage_cull_program.idx     = UINT16_MAX; }
                if (gs->foliage_indirect_program.idx != UINT16_MAX) { bgfx_destroy_program(gs->foliage_indirect_program); gs->foliage_indirect_program.idx = UINT16_MAX; }
                LOG_WARN(LOG_TAG, "foliage GPU-cull programs unavailable; foliage stays CPU-instanced");
            }
        } else {
            /* Partial load: drop whatever resolved so destroy is clean. */
            if (gs->reset_program.idx   != UINT16_MAX) { bgfx_destroy_program(gs->reset_program);   gs->reset_program.idx   = UINT16_MAX; }
            if (gs->compact_program.idx != UINT16_MAX) { bgfx_destroy_program(gs->compact_program); gs->compact_program.idx = UINT16_MAX; }
            if (gs->build_program.idx   != UINT16_MAX) { bgfx_destroy_program(gs->build_program);   gs->build_program.idx   = UINT16_MAX; }
            LOG_WARN(LOG_TAG, "indirect compute programs unavailable; using 1:1 fallback cull");
        }
    } else {
        LOG_WARN(LOG_TAG, "GPU lacks BGFX_CAPS_DRAW_INDIRECT; using 1:1 fallback cull");
    }

    LOG_SUCCESS(LOG_TAG, "GPU-driven scene online (compute cull ready, %s)",
                gs->indirect ? "indirect compaction" : "1:1 fallback");
    return gs;
}

void jce_gpu_scene_destroy(JceGpuScene *gs)
{
    if (!gs) return;
    const bool diag = getenv("JCE_GPU_SCENE_DIAG") != NULL;
#define GS_DESTROY_DIAG(kind_, handle_)                                      \
    do {                                                                     \
        if (diag) {                                                          \
            LOG_INFO(LOG_TAG, "destroy %p: %s handle=%u", (void *)gs,       \
                     (kind_), (unsigned)(handle_).idx);                       \
            jce_log_flush();                                                 \
        }                                                                    \
    } while (0)
    if (gs->cull_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("cull_program", gs->cull_program);
        bgfx_destroy_program(gs->cull_program);
    }
    if (gs->reset_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("reset_program", gs->reset_program);
        bgfx_destroy_program(gs->reset_program);
    }
    if (gs->compact_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("compact_program", gs->compact_program);
        bgfx_destroy_program(gs->compact_program);
    }
    if (gs->build_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("build_program", gs->build_program);
        bgfx_destroy_program(gs->build_program);
    }
    GS_DESTROY_DIAG("visible_buf", gs->visible_buf);
    destroy_visible_buffer(gs);
    if (gs->counter_buf.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("counter_buf", gs->counter_buf);
        bgfx_destroy_dynamic_vertex_buffer(gs->counter_buf);
    }
    if (gs->scene_buf.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("scene_buf", gs->scene_buf);
        bgfx_destroy_dynamic_vertex_buffer(gs->scene_buf);
    }
    if (gs->runmeta_gpu_buf.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("runmeta_gpu_buf", gs->runmeta_gpu_buf);
        bgfx_destroy_dynamic_vertex_buffer(gs->runmeta_gpu_buf);
    }
    if (gs->indirect_buf.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("indirect_buf", gs->indirect_buf);
        bgfx_destroy_indirect_buffer(gs->indirect_buf);
    }
#define GS_DESTROY_UNIFORM(field_)                                           \
    do {                                                                     \
        if (gs->field_.idx != UINT16_MAX) {                                  \
            GS_DESTROY_DIAG(#field_, gs->field_);                            \
            bgfx_destroy_uniform(gs->field_);                                \
        }                                                                    \
    } while (0)
    GS_DESTROY_UNIFORM(u_cull_planes);
    GS_DESTROY_UNIFORM(u_cull_params);
    GS_DESTROY_UNIFORM(u_cull_reset_params);
    GS_DESTROY_UNIFORM(u_indirect_params);
    if (gs->foliage_cull_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("foliage_cull_program", gs->foliage_cull_program);
        bgfx_destroy_program(gs->foliage_cull_program);
    }
    if (gs->foliage_indirect_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("foliage_indirect_program",
                        gs->foliage_indirect_program);
        bgfx_destroy_program(gs->foliage_indirect_program);
    }
    GS_DESTROY_UNIFORM(u_fcull_params);
    GS_DESTROY_UNIFORM(u_fcull_aabb);
    GS_DESTROY_UNIFORM(u_find_params);
    GS_DESTROY_UNIFORM(u_find_counts);
    GS_DESTROY_UNIFORM(u_mlcull_params);
    GS_DESTROY_UNIFORM(u_mlcull_model);
    if (gs->meshlet_cull_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("meshlet_cull_program", gs->meshlet_cull_program);
        bgfx_destroy_program(gs->meshlet_cull_program);
    }
    GS_DESTROY_UNIFORM(u_fcull_lod);
    GS_DESTROY_UNIFORM(u_cull_campos);
    if (gs->hiz_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("hiz_program", gs->hiz_program);
        bgfx_destroy_program(gs->hiz_program);
    }
    if (gs->hiz_reduce_program.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("hiz_reduce_program", gs->hiz_reduce_program);
        bgfx_destroy_program(gs->hiz_reduce_program);
    }
    if (gs->hiz_tex.idx != UINT16_MAX) {
        GS_DESTROY_DIAG("hiz_tex", gs->hiz_tex);
        bgfx_destroy_texture(gs->hiz_tex);
    }
    GS_DESTROY_UNIFORM(u_hiz_build);
    GS_DESTROY_UNIFORM(s_hiz);
    GS_DESTROY_UNIFORM(s_hiz_src);
    GS_DESTROY_UNIFORM(u_cull_viewproj);
    GS_DESTROY_UNIFORM(u_hiz_params);
#undef GS_DESTROY_UNIFORM
    if (diag) {
        LOG_INFO(LOG_TAG, "destroy %p: host storage", (void *)gs);
        jce_log_flush();
    }
    JCE_FREE(gs->rec);
    JCE_FREE(gs->runmeta);
    gs->alloc.free(gs, gs->alloc.ctx);
#undef GS_DESTROY_DIAG
}

bool jce_gpu_scene_is_supported(const JceGpuScene *gs)
{
    return gs && gs->supported;
}

bool jce_gpu_scene_is_indirect(const JceGpuScene *gs)
{
    return gs && gs->supported && gs->indirect;
}

uint16_t jce_gpu_scene_visible_vb(const JceGpuScene *gs)
{
    return (gs && gs->supported) ? gs->visible_buf.idx : UINT16_MAX;
}

uint16_t jce_gpu_scene_indirect_buffer(const JceGpuScene *gs)
{
    /* Only valid when cs_build_indirect actually filled the args THIS dispatch
     * (indirect_ready).  On a per-frame fall-through to the 1:1 cull the buffer
     * holds stale/uninitialised args, so report INVALID and let the draw side
     * take the 1:1 fixed-count path. */
    return (gs && gs->supported && gs->indirect && gs->indirect_ready)
        ? gs->indirect_buf.idx : UINT16_MAX;
}

/* ── foliage GPU-cull (千万 S2) ───────────────────────────────────────────
 * Cull the foliage's persistent roots VB into a compacted visible buffer +
 * single indirect draw arg.  Buffers are owned by the caller (foliage cache);
 * this drives the 3 compute dispatches (reset → cull/compact → build-indirect)
 * across the pre-color compute views.  Returns false if foliage cull is not
 * available (caps/programs) or the inputs are degenerate. */
bool jce_gpu_scene_foliage_supported(const JceGpuScene *gs)
{
    return gs && gs->supported && gs->indirect && gs->foliage_supported;
}

/* Forward decls: the foliage dispatch (below) reuses the model path's Hi-Z
 * helpers, which are defined further down. */
static bool  hiz_build(JceGpuScene *gs, uint16_t view);
static float hiz_bind_cull(JceGpuScene *gs, bool hiz_ready, uint8_t stage);

/* DIAG (JCE_FCULL_FORCE=1): drive the cull shader's force-visible lane
 * (u_fcull_params.z) — every in-range instance survives with its matrix as
 * read from b_roots, bypassing frustum/band/Hi-Z.  Backend-bug triage tool:
 * splits "roots SSBO reads zeros" (field still empty) from "a cull test
 * wrongly rejects" (field appears). */
static float fcull_force_visible(void)
{
    static int s = -1;
    if (s < 0) { const char *v = getenv("JCE_FCULL_FORCE");
                 s = (v && v[0]) ? atoi(v) : 0;
                 if (s < 0) s = 0; }
    return (float)s;
}

bool jce_gpu_scene_foliage_dispatch(JceGpuScene *gs,
                                    uint16_t reset_view, uint16_t cull_view,
                                    uint16_t roots_vb, uint16_t visible_vb,
                                    uint16_t counter_vb, uint16_t indirect_buf,
                                    uint32_t inst_count, uint32_t capacity,
                                    const jce_vec4 planes[6],
                                    jce_vec3 local_center, jce_vec3 local_extent,
                                    uint32_t num_indices)
{
    if (!jce_gpu_scene_foliage_supported(gs) || inst_count == 0u ||
        roots_vb == UINT16_MAX || visible_vb == UINT16_MAX ||
        counter_vb == UINT16_MAX || indirect_buf == UINT16_MAX) {
        return false;
    }

    bgfx_dynamic_vertex_buffer_handle_t roots   = { roots_vb };
    bgfx_dynamic_vertex_buffer_handle_t visible = { visible_vb };
    bgfx_dynamic_index_buffer_handle_t  counter = { counter_vb };
    bgfx_indirect_buffer_handle_t       indir   = { indirect_buf };

    /* Pass 1 — reset the single survivor counter (run_count = 1) on the earlier
     * reset view, so bgfx inserts a cross-view UAV barrier before the atomics. */
    float rparams[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    bgfx_set_uniform(gs->u_cull_reset_params, rparams, 1);
    bgfx_set_compute_dynamic_index_buffer(0, counter, BGFX_ACCESS_WRITE);
    bgfx_dispatch(reset_view, gs->reset_program, 1, 1, 1, BGFX_DISCARD_ALL);

    /* Hi-Z: build the (last-frame) depth pyramid on the pre-cull reset_view so the
     * foliage cull samples it across a cross-view barrier (mirrors the model
     * dispatch).  Off / no depth set → no-op, false. */
    bool hiz_ready = hiz_build(gs, reset_view);

    /* Pass 2 — cull + compact on the cull view. */
    float pl[24];
    for (int i = 0; i < 6; ++i) {
        pl[i * 4 + 0] = planes[i].x;
        pl[i * 4 + 1] = planes[i].y;
        pl[i * 4 + 2] = planes[i].z;
        pl[i * 4 + 3] = planes[i].w;
    }
    bgfx_set_uniform(gs->u_cull_planes, pl, 6);
    /* Hi-Z sampler + params + the gate (u_fcull_params.w): binds s_hiz@3 and
     * returns 1.0 to enable the occlusion test, 0.0 for frustum-only. */
    float hiz_gate = hiz_bind_cull(gs, hiz_ready, 3);
    float fparams[4] = { (float)inst_count, (float)capacity,
                         fcull_force_visible(), hiz_gate };
    bgfx_set_uniform(gs->u_fcull_params, fparams, 1);
    float aabb[8] = {
        local_center.x, local_center.y, local_center.z, 0.0f,
        local_extent.x, local_extent.y, local_extent.z, 0.0f,
    };
    bgfx_set_uniform(gs->u_fcull_aabb, aabb, 2);
    /* Single-band (no LOD banding): band_count 1 → the shader's distance/band/
     * fade logic is skipped entirely and every survivor lands in partition 0 —
     * byte-identical to the pre-S4 single-partition cull.  cap_band doubles as
     * the partition capacity. */
    float lod4[4] = { 0.0f, 1.0f, (float)capacity, 0.0f };
    bgfx_set_uniform(gs->u_fcull_lod, lod4, 1);
    float cam4[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   /* far_dist 0 = unlimited */
    bgfx_set_uniform(gs->u_cull_campos, cam4, 1);
    bgfx_set_compute_dynamic_vertex_buffer(0, roots,   BGFX_ACCESS_READ);
    bgfx_set_compute_dynamic_vertex_buffer(1, visible, BGFX_ACCESS_WRITE);
    bgfx_set_compute_dynamic_index_buffer(2, counter, BGFX_ACCESS_READWRITE);
    uint32_t groups = (inst_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
    bgfx_dispatch(cull_view, gs->foliage_cull_program, groups, 1, 1, BGFX_DISCARD_ALL);

    /* Pass 3 — build the single drawIndexedIndirect arg from the survivor count
     * (band_count 1, partition capacity = the whole buffer). */
    float iparams[4] = { 1.0f, (float)capacity, 0.0f, 0.0f };
    bgfx_set_uniform(gs->u_find_params, iparams, 1);
    float icounts[8] = { (float)num_indices, 0, 0, 0, 0, 0, 0, 0 };
    bgfx_set_uniform(gs->u_find_counts, icounts, 2);
    bgfx_set_compute_dynamic_index_buffer(0, counter, BGFX_ACCESS_READ);
    bgfx_set_compute_indirect_buffer(1, indir, BGFX_ACCESS_WRITE);
    bgfx_dispatch(cull_view, gs->foliage_indirect_program, 1, 1, 1, BGFX_DISCARD_ALL);
    return true;
}

/* ── 千万 S4/S5: banded LOD-in-cull, tile-aware (begin / tile / end) ──────
 * One dispatch per (resident, frustum-visible) TILE culls + LOD-classifies its
 * instances into the SHARED band partitions of one visible buffer; the atomics
 * make cross-tile appends safe.  begin resets the band counters (cross-view
 * UAV barrier) + builds the Hi-Z pyramid once; end builds one indirect element
 * per band (startInstance = band * cap_band).  A non-tiled scatter is simply
 * begin + one tile + end. */

bool jce_gpu_scene_foliage_lod_begin(JceGpuScene *gs, uint16_t reset_view,
                                     uint16_t counter_vb, uint32_t band_count,
                                     bool *out_hiz_ready)
{
    if (!jce_gpu_scene_foliage_supported(gs) || band_count == 0u ||
        counter_vb == UINT16_MAX)
        return false;
    bgfx_dynamic_vertex_buffer_handle_t counter = { counter_vb };
    float rparams[4] = { (float)band_count, 0.0f, 0.0f, 0.0f };
    bgfx_set_uniform(gs->u_cull_reset_params, rparams, 1);
    bgfx_set_compute_dynamic_vertex_buffer(0, counter, BGFX_ACCESS_WRITE);
    bgfx_dispatch(reset_view, gs->reset_program, 1, 1, 1, BGFX_DISCARD_ALL);
    bool hiz_ready = hiz_build(gs, reset_view);
    if (out_hiz_ready) *out_hiz_ready = hiz_ready;
    return true;
}

void jce_gpu_scene_foliage_lod_tile(JceGpuScene *gs, uint16_t cull_view,
                                    uint16_t roots_vb, uint32_t inst_count,
                                    uint16_t visible_vb, uint16_t counter_vb,
                                    const jce_vec4 planes[6],
                                    jce_vec3 local_center, jce_vec3 local_extent,
                                    jce_vec3 cam_pos, float far_dist,
                                    float band_step, uint32_t band_count,
                                    uint32_t cap_band, float fade_w,
                                    bool hiz_ready)
{
    if (!gs || inst_count == 0u || roots_vb == UINT16_MAX ||
        visible_vb == UINT16_MAX || counter_vb == UINT16_MAX)
        return;
    bgfx_dynamic_vertex_buffer_handle_t roots   = { roots_vb };
    bgfx_dynamic_vertex_buffer_handle_t visible = { visible_vb };
    bgfx_dynamic_vertex_buffer_handle_t counter = { counter_vb };

    float pl[24];
    for (int i = 0; i < 6; ++i) {
        pl[i * 4 + 0] = planes[i].x; pl[i * 4 + 1] = planes[i].y;
        pl[i * 4 + 2] = planes[i].z; pl[i * 4 + 3] = planes[i].w;
    }
    bgfx_set_uniform(gs->u_cull_planes, pl, 6);
    float hiz_gate = hiz_bind_cull(gs, hiz_ready, 3);
    float fparams[4] = { (float)inst_count, 0.0f,
                         fcull_force_visible(), hiz_gate };
    bgfx_set_uniform(gs->u_fcull_params, fparams, 1);
    float aabb[8] = {
        local_center.x, local_center.y, local_center.z, 0.0f,
        local_extent.x, local_extent.y, local_extent.z, 0.0f,
    };
    bgfx_set_uniform(gs->u_fcull_aabb, aabb, 2);
    float lod4[4] = { band_step, (float)band_count, (float)cap_band, fade_w };
    bgfx_set_uniform(gs->u_fcull_lod, lod4, 1);
    float cam4[4] = { cam_pos.x, cam_pos.y, cam_pos.z, far_dist };
    bgfx_set_uniform(gs->u_cull_campos, cam4, 1);
    bgfx_set_compute_dynamic_vertex_buffer(0, roots,   BGFX_ACCESS_READ);
    bgfx_set_compute_dynamic_vertex_buffer(1, visible, BGFX_ACCESS_WRITE);
    bgfx_set_compute_dynamic_vertex_buffer(2, counter, BGFX_ACCESS_READWRITE);
    uint32_t groups = (inst_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
    bgfx_dispatch(cull_view, gs->foliage_cull_program, groups, 1, 1, BGFX_DISCARD_ALL);
}

bool jce_gpu_scene_foliage_lod_end(JceGpuScene *gs, uint16_t cull_view,
                                   uint16_t counter_vb, uint16_t indirect_buf,
                                   uint32_t band_count, uint32_t cap_band,
                                   const uint32_t *band_num_indices)
{
    if (!gs || band_count == 0u || counter_vb == UINT16_MAX ||
        indirect_buf == UINT16_MAX)
        return false;
    if (band_count > 8u) band_count = 8u;
    bgfx_dynamic_vertex_buffer_handle_t counter = { counter_vb };
    bgfx_indirect_buffer_handle_t       indir   = { indirect_buf };
    float iparams[4] = { (float)band_count, (float)cap_band, 0.0f, 0.0f };
    bgfx_set_uniform(gs->u_find_params, iparams, 1);
    float icounts[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (uint32_t b = 0; b < band_count; ++b)
        icounts[b] = (float)band_num_indices[b];
    bgfx_set_uniform(gs->u_find_counts, icounts, 2);
    bgfx_set_compute_dynamic_vertex_buffer(0, counter, BGFX_ACCESS_READ);
    bgfx_set_compute_indirect_buffer(1, indir, BGFX_ACCESS_WRITE);
    bgfx_dispatch(cull_view, gs->foliage_indirect_program, 1, 1, 1, BGFX_DISCARD_ALL);
    return true;
}

/* ── Nanite-lite V2: per-meshlet cluster cull ─────────────────────────── */

bool jce_gpu_scene_meshlet_supported(const JceGpuScene *gs)
{
    return gs && gs->meshlet_supported;
}

/* V3.1: build (or refresh) the Hi-Z pyramid for this frame's meshlet
 * dispatches and report readiness.  The scene renderer calls this ONCE per
 * color pass (its own pass guard) — per-entity dispatches then just bind.
 * JCE_MESHLET_HIZ=0 opts the meshlet path out without touching foliage. */
bool jce_gpu_scene_meshlet_hiz_prepare(JceGpuScene *gs, uint16_t view)
{
    static int s_on = -1;
    if (s_on < 0) { const char *v = getenv("JCE_MESHLET_HIZ");
                    s_on = (!v || !v[0]) ? 1 : (v[0] != '0'); }
    if (!s_on || !gs || !gs->meshlet_supported) return false;
    return hiz_build(gs, view);
}

void jce_gpu_scene_meshlet_dispatch(JceGpuScene *gs, uint16_t cull_view,
                                    uint16_t meshlet_vb, uint32_t count,
                                    float max_scale, const jce_mat4 *world,
                                    const jce_vec4 planes[6], jce_vec3 cam_pos,
                                    float err_k, bool hiz_ready,
                                    bool shadow_mode,
                                    uint16_t indirect_buf)
{
    if (!gs || !gs->meshlet_supported || count == 0u ||
        meshlet_vb == UINT16_MAX || indirect_buf == UINT16_MAX || !world)
        return;
    bgfx_vertex_buffer_handle_t   mlvb = { meshlet_vb };
    bgfx_indirect_buffer_handle_t ind  = { indirect_buf };

    float pl[24];
    for (int i = 0; i < 6; ++i) {
        pl[i * 4 + 0] = planes[i].x; pl[i * 4 + 1] = planes[i].y;
        pl[i * 4 + 2] = planes[i].z; pl[i * 4 + 3] = planes[i].w;
    }
    bgfx_set_uniform(gs->u_cull_planes, pl, 6);
    /* .w = V3 DAG-cut error allowance per unit distance (<= 0 = leaves). */
    float cam4[4] = { cam_pos.x, cam_pos.y, cam_pos.z, err_k };
    bgfx_set_uniform(gs->u_cull_campos, cam4, 1);
    /* JCE_MLCULL_DIAG bisect lane: 1 = frustum only, 2 = cone only (both
     * evaluate on the LEAF cluster set with Hi-Z off — the V3 cut and the
     * pyramid are bypassed so the isolated test is the only visibility
     * variable).  params.w packs {diag, hiz}: w = diag + 8*hiz. */
    static float s_ml_diag = -1.0f;
    if (s_ml_diag < 0.0f) {
        const char *dv = getenv("JCE_MLCULL_DIAG");
        s_ml_diag = (dv && dv[0]) ? (float)atof(dv) : 0.0f;
    }
    /* Shadow mode forces Hi-Z off (no light-space pyramid) and adds the
     * +16 shadow bit; the shader then keeps frustum but drops cone+Hi-Z. */
    float hiz_gate = shadow_mode ? 0.0f : hiz_bind_cull(gs, hiz_ready, 2);
    float mp[4] = { (float)count, max_scale, fcull_force_visible(),
                    s_ml_diag + (hiz_gate > 0.5f ? 8.0f : 0.0f)
                              + (shadow_mode ? 16.0f : 0.0f) };
    bgfx_set_uniform(gs->u_mlcull_params, mp, 1);
    bgfx_set_uniform(gs->u_mlcull_model, world->raw[0], 1);
    bgfx_set_compute_vertex_buffer(0, mlvb, BGFX_ACCESS_READ);
    bgfx_set_compute_indirect_buffer(1, ind, BGFX_ACCESS_WRITE);
    uint32_t groups = (count + 63u) / 64u;
    bgfx_dispatch(cull_view, gs->meshlet_cull_program, groups, 1, 1,
                  BGFX_DISCARD_ALL);
}

/* ── per-frame batch ──────────────────────────────────────────────────── */

void jce_gpu_scene_begin(JceGpuScene *gs)
{
    if (!gs) return;
    memset(&gs->frame_stats, 0, sizeof(gs->frame_stats));
    gs->frame_stats.supported = gs->supported;
    gs->frame_stats.indirect_supported = gs->indirect;
    gs->rec_count = 0;
    gs->group_count = 0;
    gs->run_count = 0;
    gs->indirect_ready = false;  /* no valid indirect args until pass-3 runs */
}

bool jce_gpu_scene_add_draw_group(
    JceGpuScene *gs, const JceGpuSceneRecord *records, uint32_t count,
    const uint32_t *num_indices, uint32_t draw_count,
    JceGpuSceneDrawGroup *out_group)
{
    uint32_t base;
    uint32_t group_index;
    uint32_t first_run;

    if (!gs || !gs->supported || !records || count == 0u ||
        !num_indices || draw_count == 0u)
        return false;
    if (gs->group_count == UINT32_MAX ||
        count > MAX_SCENE_RECORDS - gs->rec_count ||
        draw_count > MAX_DRAW_METADATA - gs->run_count)
        return false;

    base = gs->rec_count;
    group_index = gs->group_count;
    first_run = gs->run_count;

    if (base + count > gs->rec_cap) {
        uint32_t need = base + count;
        uint32_t nc = gs->rec_cap ? gs->rec_cap : 1024u;
        while (nc < need) {
            if (nc > UINT32_MAX / 2u) {
                nc = need;
                break;
            }
            nc *= 2u;
        }
        JceGpuSceneRecord *nr = (JceGpuSceneRecord *)JCE_REALLOC(
            gs->rec, (size_t)nc * sizeof(JceGpuSceneRecord));
        if (!nr) return false;
        gs->rec = nr;
        gs->rec_cap = nc;
    }

    /* Reserve every primitive's metadata before mutating counts, making this
     * append transactional under allocation failure. */
    if (gs->indirect && first_run + draw_count > gs->runmeta_cap) {
        uint32_t need = first_run + draw_count;
        uint32_t nc = gs->runmeta_cap ? gs->runmeta_cap : 256u;
        while (nc < need) {
            if (nc > UINT32_MAX / 2u) {
                nc = need;
                break;
            }
            nc *= 2u;
        }
        void *nm = JCE_REALLOC(gs->runmeta, (size_t)nc * sizeof(*gs->runmeta));
        if (!nm) return false;
        gs->runmeta = nm;
        gs->runmeta_cap = nc;
    }

    /* Copy records once. Every primitive draw shares this compacted visibility
     * partition and its survivor counter. */
    for (uint32_t i = 0; i < count; i++) {
        JceGpuSceneRecord r = records[i];
        r.run_base  = (float)base;
        r.run_index = (float)group_index;
        gs->rec[base + i] = r;
    }

    if (gs->indirect) {
        for (uint32_t draw = 0; draw < draw_count; draw++) {
            uint32_t run = first_run + draw;
            gs->runmeta[run].num_indices = num_indices[draw];
            gs->runmeta[run].run_base = base;
            gs->runmeta[run].counter_index = group_index;
        }
    }

    gs->rec_count += count;
    gs->group_count += 1u;
    gs->run_count += draw_count;
    gs->frame_stats.records = gs->rec_count;
    gs->frame_stats.groups = gs->group_count;
    gs->frame_stats.runs = gs->run_count;

    if (out_group) {
        out_group->run_base = base;
        out_group->first_indirect_el =
            gs->indirect ? first_run : UINT32_MAX;
        out_group->draw_count = draw_count;
    }
    return true;
}

bool jce_gpu_scene_add_run(JceGpuScene *gs, const JceGpuSceneRecord *records,
                           uint32_t count, uint32_t num_indices,
                           JceGpuSceneRun *out_run)
{
    JceGpuSceneDrawGroup group;

    if (!jce_gpu_scene_add_draw_group(
            gs, records, count, &num_indices, 1u, &group))
        return false;

    if (out_run) {
        out_run->run_base = group.run_base;
        out_run->indirect_el = group.first_indirect_el;
    }
    return true;
}

/* ── dispatch ─────────────────────────────────────────────────────────── */

/* Upload the whole batch's scene records into the persistent COMPUTE_READ scene
 * buffer (NOT a transient VB — that has no D3D11 SRV; see the scene_buf field
 * comment).  Returns false (caller falls back to CPU) on buffer-create failure.
 * On success writes *out_off_vec4 = 0 (the SRV starts at element 0 — no rebase). */
static bool upload_scene_records(JceGpuScene *gs, float *out_off_vec4)
{
    if (!ensure_scene_capacity(gs, gs->rec_count)) return false;

    const bgfx_memory_t *mem =
        bgfx_copy(gs->rec, (uint32_t)((size_t)gs->rec_count * SCENE_STRIDE_BYTES));
    if (!mem) return false;
    bgfx_update_dynamic_vertex_buffer(gs->scene_buf, 0, mem);
    gs->frame_stats.upload_calls++;
    gs->frame_stats.uploaded_bytes +=
        (uint64_t)gs->rec_count * SCENE_STRIDE_BYTES;

    *out_off_vec4 = 0.0f;   /* persistent buffer: records start at element 0 */
    return true;
}

/* Per-frame Hi-Z inputs. The cull runs before this frame's depth prepass, so
 * `depth` + `prev_vp` are LAST frame's (1-frame-late, conservative). Off-path:
 * enable=false leaves hiz_on false → the cull's u_cull_params.w stays 0. */
void jce_gpu_scene_set_hiz(JceGpuScene *gs, uint16_t depth_tex,
                           const jce_mat4 *prev_vp, uint16_t view_w,
                           uint16_t view_h, bool enable)
{
    if (!gs) return;
    gs->hiz_on = enable && gs->hiz_supported && depth_tex != UINT16_MAX &&
                 prev_vp && view_w > 0 && view_h > 0;
    if (gs->hiz_on) {
        gs->hiz_src_depth.idx = depth_tex;
        gs->hiz_prev_vp       = *prev_vp;
        gs->hiz_view_w        = view_w;
        gs->hiz_view_h        = view_h;
    }
}

/* Build the Hi-Z MAX-depth pyramid from the (last-frame) depth prepass, on the
 * pre-cull reset_view so its writes are cross-view-barriered before the cull
 * samples it.  mip 0 = 2x2-MAX of the depth (half-viewport, cs_hiz_build,
 * sampler-read); mips 1..N-1 = 2x2-MAX of the previous mip (cs_hiz_reduce,
 * image-read, NO SRV/UAV aliasing).  The cull picks mip = ceil(log2(footprint
 * span)) so LARGE footprints test coarse mips → occlude big instances (the whole
 * point of the pyramid vs the single level, which could only test <=1-texel
 * footprints).  If cs_hiz_reduce is unavailable, falls back to a single level
 * (num_mips=1).  Returns true when the pyramid is ready to bind. */
static bool hiz_build(JceGpuScene *gs, uint16_t view)
{
    if (!gs->hiz_on || gs->hiz_program.idx == UINT16_MAX) return false;
    uint16_t w = (uint16_t)(gs->hiz_view_w / 2u); if (w < 1u) w = 1u;
    uint16_t h = (uint16_t)(gs->hiz_view_h / 2u); if (h < 1u) h = 1u;
    /* Full mip chain down to 1x1 (so any footprint has a mip), unless the reduce
     * kernel is missing → single level. */
    uint8_t want_mips = 1;
    if (gs->hiz_reduce_program.idx != UINT16_MAX) {
        uint16_t m = (w > h) ? w : h;
        while (m > 1u) { m >>= 1; want_mips++; }
    }
    if (gs->hiz_tex.idx == UINT16_MAX || gs->hiz_w != w || gs->hiz_h != h
        || gs->hiz_mips != want_mips) {
        if (gs->hiz_tex.idx != UINT16_MAX) bgfx_destroy_texture(gs->hiz_tex);
        gs->hiz_tex = bgfx_create_texture_2d(
            w, h, want_mips > 1, 1, BGFX_TEXTURE_FORMAT_R32F,
            BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_MIN_POINT |
            BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT |
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL, 0);
        gs->hiz_w = w; gs->hiz_h = h; gs->hiz_mips = want_mips;
    }
    if (gs->hiz_tex.idx == UINT16_MAX) { gs->hiz_on = false; return false; }

    /* mip 0 from the depth prepass (sampler read). */
    float bp0[4] = { (float)w, (float)h, 0.0f, 0.0f };
    bgfx_set_uniform(gs->u_hiz_build, bp0, 1);
    bgfx_set_texture(0, gs->s_hiz_src, gs->hiz_src_depth, UINT32_MAX);
    bgfx_set_image(1, gs->hiz_tex, 0, BGFX_ACCESS_WRITE, BGFX_TEXTURE_FORMAT_R32F);
    bgfx_dispatch(view, gs->hiz_program, (w + 7u) / 8u, (h + 7u) / 8u, 1,
                  BGFX_DISCARD_ALL);
    gs->frame_stats.compute_dispatches++;

    /* mips 1..N-1 from the previous mip (image read → image write; distinct mips
     * of the same texture, so no SRV/UAV aliasing).  Same view: on D3D11 the
     * driver serialises consecutive dispatches, so mip i-1's writes are visible to
     * mip i's reads. */
    for (uint8_t lvl = 1; lvl < gs->hiz_mips; ++lvl) {
        uint16_t mw = (uint16_t)(w >> lvl); if (mw < 1u) mw = 1u;
        uint16_t mh = (uint16_t)(h >> lvl); if (mh < 1u) mh = 1u;
        float bp[4] = { (float)mw, (float)mh, 0.0f, 0.0f };
        bgfx_set_uniform(gs->u_hiz_build, bp, 1);
        bgfx_set_image(0, gs->hiz_tex, (uint8_t)(lvl - 1u), BGFX_ACCESS_READ,
                       BGFX_TEXTURE_FORMAT_R32F);
        bgfx_set_image(1, gs->hiz_tex, lvl, BGFX_ACCESS_WRITE,
                       BGFX_TEXTURE_FORMAT_R32F);
        bgfx_dispatch(view, gs->hiz_reduce_program, (mw + 7u) / 8u, (mh + 7u) / 8u,
                      1, BGFX_DISCARD_ALL);
        gs->frame_stats.compute_dispatches++;
    }
    if (getenv("JCE_HIZ_DIAG")) {
        static uint32_t s_f = 0;
        if ((s_f++ % 60u) == 0u)
            LOG_INFO(LOG_TAG, "Hi-Z pyramid built: %ux%u r32f, %u mips from depth %u (call #%u)",
                     (unsigned)w, (unsigned)h, (unsigned)gs->hiz_mips,
                     (unsigned)gs->hiz_src_depth.idx, (unsigned)s_f);
    }
    return true;
}

/* Bind the Hi-Z sampler + params for a cull dispatch and return params[3]
 * (1.0 = test occlusion, 0.0 = frustum-only). Call right before the dispatch. */
static float hiz_bind_cull(JceGpuScene *gs, bool hiz_ready, uint8_t stage)
{
    if (!hiz_ready) return 0.0f;
    bgfx_set_texture(stage, gs->s_hiz, gs->hiz_tex, UINT32_MAX);
    bgfx_set_uniform(gs->u_cull_viewproj, gs->hiz_prev_vp.raw, 1);
    /* num_mips (hp.z) gates the cull: it tests footprints up to mip = num_mips-1.
     * The full pyramid (mips down to 1x1) lets it occlude large-footprint
     * instances; a single level restricts it to <=1-texel footprints. */
    float hp[4] = { (float)gs->hiz_w, (float)gs->hiz_h, (float)gs->hiz_mips, 0.0f };
    bgfx_set_uniform(gs->u_hiz_params, hp, 1);
    return 1.0f;
}

bool jce_gpu_scene_dispatch(JceGpuScene *gs, uint16_t reset_view,
                            uint16_t cull_view, const jce_vec4 planes[6])
{
    if (!gs || !gs->supported || !planes || gs->rec_count == 0) return false;

    JCE_PROFILE_ZONE_N("GpuScene::Dispatch");
    gs->frame_stats.dispatches++;
    gs->frame_stats.hiz_enabled = gs->hiz_on;

    /* No valid indirect args until pass-3 runs this dispatch (defensive: also
     * cleared in begin(), but a fall-through to the 1:1 cull below must leave the
     * draw side seeing an INVALID indirect buffer). */
    gs->indirect_ready = false;

    /* Hi-Z: build the (last-frame) depth pyramid on the pre-cull reset_view so
     * the cull samples it across a cross-view barrier. Off → no-op, false. */
    bool hiz_ready = hiz_build(gs, reset_view);

    if (!ensure_capacity(gs, gs->rec_count)) {
        JCE_PROFILE_ZONE_END;
        return false;
    }

    float scene_off_vec4;
    if (!upload_scene_records(gs, &scene_off_vec4)) {
        JCE_PROFILE_ZONE_END;
        return false;
    }

    /* Frustum planes uniform (shared by both paths). */
    float pl[24];
    for (int i = 0; i < 6; i++) {
        pl[i * 4 + 0] = planes[i].x;
        pl[i * 4 + 1] = planes[i].y;
        pl[i * 4 + 2] = planes[i].z;
        pl[i * 4 + 3] = planes[i].w;
    }

    /* ── INDIRECT compaction path ─────────────────────────────────────── */
    if (gs->indirect) {
        /* Indirect needs one counter per visibility group and one metadata plus
         * indirect element per primitive draw. If any prerequisite fails, fall
         * through to the correct 1:1 path for this dispatch. */
        bool ok = ensure_counter_capacity(gs, gs->group_count)
               && ensure_indirect_capacity(gs, gs->run_count)
               && ensure_runmeta_gpu_capacity(gs, gs->run_count);

        float runmeta_off_vec4 = 0.0f;
        if (ok) {
            /* Pack draw meta as vec4 FLOAT values
             * (numIndices, run_base, counter_index, 0)
             * into the persistent COMPUTE_READ draw-metadata buffer (a transient VB
             * has no D3D11 SRV — see scene_buf).  bgfx exposes a vertex buffer's
             * compute SRV as RGBA32F, so the shader reads vec4 and uint()'s it.
             * Counts/bases are < 2^24 in any real scene → the float round-trip is
             * exact.  bgfx_alloc gives a bgfx-owned block we fill in place. */
            const bgfx_memory_t *mem =
                bgfx_alloc((uint32_t)((size_t)gs->run_count * RUNMETA_STRIDE_BYTES));
            if (!mem) {
                ok = false;
            } else {
                float *dst = (float *)mem->data;
                for (uint32_t r = 0; r < gs->run_count; r++) {
                    dst[r * 4 + 0] = (float)gs->runmeta[r].num_indices;
                    dst[r * 4 + 1] = (float)gs->runmeta[r].run_base;
                    dst[r * 4 + 2] = (float)gs->runmeta[r].counter_index;
                    dst[r * 4 + 3] = 0.0f;
                }
                bgfx_update_dynamic_vertex_buffer(gs->runmeta_gpu_buf, 0, mem);
                gs->frame_stats.upload_calls++;
                gs->frame_stats.uploaded_bytes +=
                    (uint64_t)gs->run_count * RUNMETA_STRIDE_BYTES;
                runmeta_off_vec4 = 0.0f;   /* persistent: starts at element 0 */
            }
        }

        if (ok) {
            /* Pass 1: reset per-group counters. Dispatched on a separate, earlier
             * compute view (reset_view, ordered before cull_view by the view-order
             * builder).  This is required for correctness on D3D12: reset binds the
             * counter ACCESS_WRITE and compact binds it ACCESS_READWRITE — both are
             * the UAV state, so bgfx-D3D12 emits NO barrier between two dispatches
             * on the SAME view (it only barriers on a state CHANGE), letting
             * compact's atomicAdd race reset's zero.  bgfx serialises compute
             * across DIFFERENT views (cross-view barrier), so putting reset on its
             * own earlier view guarantees the zero is visible to compact's atomics.
             * (Vulkan/GL emit a per-dispatch global compute barrier, so they were
             * already safe; this also fixes D3D12, the default Windows backend.) */
            {
                float rparams[4] = {
                    (float)gs->group_count, 0.0f, 0.0f, 0.0f
                };
                bgfx_set_uniform(gs->u_cull_reset_params, rparams, 1);
                bgfx_set_compute_dynamic_vertex_buffer(0, gs->counter_buf, BGFX_ACCESS_WRITE);
                uint32_t groups =
                    (gs->group_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
                bgfx_dispatch(reset_view, gs->reset_program, groups, 1, 1, BGFX_DISCARD_ALL);
                gs->frame_stats.compute_dispatches++;
            }
            /* Pass 2: cull + compact survivors into their run partitions. */
            {
                float params[4] = { (float)gs->rec_count, (float)gs->capacity,
                                    scene_off_vec4, 0.0f };
                params[3] = hiz_bind_cull(gs, hiz_ready, 3);   /* s_hiz @ stage 3 */
                bgfx_set_uniform(gs->u_cull_planes, pl, 6);
                bgfx_set_uniform(gs->u_cull_params, params, 1);
                bgfx_set_compute_dynamic_vertex_buffer(0, gs->scene_buf, BGFX_ACCESS_READ);
                bgfx_set_compute_dynamic_vertex_buffer(1, gs->visible_buf, BGFX_ACCESS_WRITE);
                bgfx_set_compute_dynamic_vertex_buffer(2, gs->counter_buf, BGFX_ACCESS_READWRITE);
                uint32_t groups = (gs->rec_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
                bgfx_dispatch(cull_view, gs->compact_program, groups, 1, 1, BGFX_DISCARD_ALL);
                gs->frame_stats.compute_dispatches++;
            }
            /* Pass 3: build one indirect argument per primitive draw. */
            {
                float iparams[4] = { (float)gs->run_count, runmeta_off_vec4, 0.0f, 0.0f };
                bgfx_set_uniform(gs->u_indirect_params, iparams, 1);
                bgfx_set_compute_dynamic_vertex_buffer(0, gs->runmeta_gpu_buf, BGFX_ACCESS_READ);
                bgfx_set_compute_dynamic_vertex_buffer(1, gs->counter_buf, BGFX_ACCESS_READ);
                bgfx_set_compute_indirect_buffer(2, gs->indirect_buf, BGFX_ACCESS_WRITE);
                uint32_t groups = (gs->run_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
                bgfx_dispatch(cull_view, gs->build_program, groups, 1, 1, BGFX_DISCARD_ALL);
                gs->frame_stats.compute_dispatches++;
            }
            /* Pass 3 produced valid arguments for every primitive draw. */
            gs->indirect_ready = true;
            gs->frame_stats.indirect_ready = true;
            gs->frame_stats.dispatch_succeeded = true;
            JCE_PROFILE_ZONE_END;
            return true;
        }
        /* else: indirect prerequisites failed this frame — fall through to the
         * 1:1 cull below so nothing is dropped (the draw side detects the missing
         * indirect buffer and uses the fixed-count path). */
    }

    /* ── FALLBACK 1:1 cull path ───────────────────────────────────────── */
    {
        float params[4] = { (float)gs->rec_count, (float)gs->capacity,
                            scene_off_vec4, 0.0f };
        params[3] = hiz_bind_cull(gs, hiz_ready, 2);   /* s_hiz @ stage 2 (frustum) */
        bgfx_set_uniform(gs->u_cull_planes, pl, 6);
        bgfx_set_uniform(gs->u_cull_params, params, 1);
        bgfx_set_compute_dynamic_vertex_buffer(0, gs->scene_buf, BGFX_ACCESS_READ);
        bgfx_set_compute_dynamic_vertex_buffer(1, gs->visible_buf, BGFX_ACCESS_WRITE);
        uint32_t groups = (gs->rec_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
        bgfx_dispatch(cull_view, gs->cull_program, groups, 1, 1, BGFX_DISCARD_ALL);
        gs->frame_stats.compute_dispatches++;
    }

    gs->frame_stats.dispatch_succeeded = true;
    JCE_PROFILE_ZONE_END;
    return true;
}

void jce_gpu_scene_get_frame_stats(const JceGpuScene *gs,
                                   JceGpuSceneFrameStats *out_stats)
{
    if (!out_stats)
        return;
    if (!gs) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    *out_stats = gs->frame_stats;
}
