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
 *     1. cs_cull_reset    : zero the per-run survivor counters (GPU clear; no
 *                           CPU bgfx_update_dynamic_* -> avoids the D3D12 staging
 *                           NULL-deref under pressure).
 *     2. cs_cull_compact  : one thread per record; survivors atomicAdd into their
 *                           run counter and write their mat4 DENSELY into the
 *                           run's partition of the visible buffer.  Culled records
 *                           write nothing -> no degenerate zero-area instances.
 *     3. cs_build_indirect: one thread per run; reads the survivor counter + the
 *                           run's mesh index count and writes the run's
 *                           drawIndexedIndirect args (numInstances = survivors).
 *   The renderer then issues ONE bgfx_submit_indirect per run.  This removes the
 *   degenerate-slot raster waste AND the CPU per-run fixed-count submit cost of
 *   the fallback path below.
 *
 * FALLBACK 1:1 (no BGFX_CAPS_DRAW_INDIRECT): the redesigned cs_cull_frustum
 *   writes EVERY visible-buffer slot 1:1 (record id -> slot id); survivors get
 *   their world matrix and culled records get a ZERO matrix (degenerate
 *   instance).  No atomic counter, no indirect buffer; the caller draws each run
 *   with a CPU fixed-count submit over [run_base, run_base+count).
 *
 * STAGING-FREE DESIGN (no per-frame bgfx_update_dynamic_* on compute buffers)
 * --------------------------------------------------------------------------
 * Per-frame data (scene records, run meta) rides TRANSIENT buffers (bgfx uploads
 * the whole transient ring with ONE staging allocation per frame), and the
 * persistent visible/counter buffers are written only by compute / cleared by the
 * GPU reset pass.  jce_gpu_scene_dispatch makes ZERO bgfx_update_dynamic_* calls.
 *
 * One per-frame BATCH covers the whole color-pass instanced set; bgfx orders the
 * compute view ahead of the color view and inserts UAV barriers between the three
 * same-view compute dispatches, so the indirect args + compact instances are
 * resident before the first draw.
 *
 * Layout (must mirror the .sc shaders):
 *   scene_buf  (transient VB) : 7 vec4 per record (stride 112 B), bound RO,
 *                               ring-rebased by u_cull_params.z (vec4 offset).
 *   visible_buf (dynamic VB)  : 4 vec4 per slot (a mat4 instance stream),
 *                               COMPUTE_READ_WRITE; compact per run (indirect) or
 *                               1:1 slot==record id (fallback).
 *   counter_buf (dynamic VB)  : 1 uint per run, COMPUTE_READ_WRITE (indirect only)
 *   runmeta_buf (transient VB): 1 uvec4 per run (numIndices, run_base, _, _)
 *   indirect_buf (indirect)   : 1 element per run (2 uvec4 drawIndexedIndirect)
 */

#include <jce/renderer/jce_gpu_scene.h>
#include <jce/renderer/jce_shaders.h>   /* embedded engine pak fallback */
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <stdio.h>
#include <string.h>

#define LOG_TAG "gpu-scene"

#define CULL_THREADS_X      64u
#define SCENE_VEC4_PER_REC  7u
#define SCENE_STRIDE_BYTES  (SCENE_VEC4_PER_REC * 16u)   /* 112 B */
#define VIS_VEC4_PER_SLOT   4u
#define VIS_STRIDE_BYTES    (VIS_VEC4_PER_SLOT * 16u)    /* 64 B (mat4) */
#define RUNMETA_STRIDE_BYTES 16u                         /* one uvec4 per run */

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

    /* Persistent per-run survivor counter buffer (indirect path).  One uint per
     * run; zeroed by cs_cull_reset, atomicAdd'd by cs_cull_compact. */
    bgfx_dynamic_vertex_buffer_handle_t counter_buf;
    uint32_t        counter_cap;     /* runs the counter buffer can hold */

    /* Indirect draw-args buffer (indirect path).  One element per run. */
    bgfx_indirect_buffer_handle_t indirect_buf;
    uint32_t        indirect_cap;    /* runs the indirect buffer can hold */

    bgfx_vertex_layout_t scene_layout;     /* 7 vec4 (transient scene records) */
    bgfx_vertex_layout_t visible_layout;   /* 4 vec4 (instance mat4)           */
    bgfx_vertex_layout_t counter_layout;   /* 1 uint per run                   */
    bgfx_vertex_layout_t runmeta_layout;   /* 1 uvec4 per run                  */

    bgfx_uniform_handle_t u_cull_planes;        /* vec4[6] */
    bgfx_uniform_handle_t u_cull_params;        /* vec4    */
    bgfx_uniform_handle_t u_cull_reset_params;  /* vec4    */
    bgfx_uniform_handle_t u_indirect_params;    /* vec4    */

    /* Per-frame batch (host scratch accumulated by add_run, uploaded by dispatch
     * into transient VBs). */
    JceGpuSceneRecord *rec;
    uint32_t           rec_count;
    uint32_t           rec_cap;

    /* Per-run mesh index counts (indirect path): runmeta[r] = (num_indices,
     * run_base).  Index by run id. */
    struct { uint32_t num_indices; uint32_t run_base; } *runmeta;
    uint32_t           run_count;    /* number of runs appended this frame */
    uint32_t           runmeta_cap;
};

/* ── shader loading (mirrors jce_gpu_particles.c) ─────────────────────── */

static const char *backend_suffix(void)
{
    switch (bgfx_get_renderer_type()) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12: return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:     return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:     return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:   return "essl";
    case BGFX_RENDERER_TYPE_METAL:      return "mtl";
    default:                            return NULL;
    }
}

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

/* Ensure the visible buffer holds at least `need` slots.  Grows geometrically;
 * not shrunk (steady-state size stabilises).  This is a Default-heap resource
 * created once per growth (createCommittedResource at create time only — NOT a
 * per-frame staging allocation), so it never hits the per-call Upload-heap
 * staging path that crashed. */
static bool ensure_capacity(JceGpuScene *gs, uint32_t need)
{
    if (need <= gs->capacity && gs->visible_buf.idx != UINT16_MAX) return true;

    uint32_t cap = gs->capacity ? gs->capacity : 1024u;
    while (cap < need) cap *= 2u;
    cap = (cap + CULL_THREADS_X - 1u) & ~(CULL_THREADS_X - 1u);

    destroy_visible_buffer(gs);

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

/* Ensure the per-run counter buffer holds at least `runs` uints (indirect path).
 * One uint per run; written/cleared only by compute. */
static bool ensure_counter_capacity(JceGpuScene *gs, uint32_t runs)
{
    if (runs <= gs->counter_cap && gs->counter_buf.idx != UINT16_MAX) return true;

    uint32_t cap = gs->counter_cap ? gs->counter_cap : 256u;
    while (cap < runs) cap *= 2u;
    cap = (cap + CULL_THREADS_X - 1u) & ~(CULL_THREADS_X - 1u);

    if (gs->counter_buf.idx != UINT16_MAX) {
        bgfx_destroy_dynamic_vertex_buffer(gs->counter_buf);
        gs->counter_buf.idx = UINT16_MAX;
        gs->counter_cap = 0;
    }
    gs->counter_buf = bgfx_create_dynamic_vertex_buffer(
        cap, &gs->counter_layout,
        BGFX_BUFFER_COMPUTE_READ_WRITE
        | BGFX_BUFFER_COMPUTE_FORMAT_32X1
        | BGFX_BUFFER_COMPUTE_TYPE_UINT);
    if (gs->counter_buf.idx == UINT16_MAX) return false;
    gs->counter_cap = cap;
    return true;
}

/* Ensure the indirect buffer holds at least `runs` draw elements. */
static bool ensure_indirect_capacity(JceGpuScene *gs, uint32_t runs)
{
    if (runs <= gs->indirect_cap && gs->indirect_buf.idx != UINT16_MAX) return true;

    uint32_t cap = gs->indirect_cap ? gs->indirect_cap : 256u;
    while (cap < runs) cap *= 2u;

    if (gs->indirect_buf.idx != UINT16_MAX) {
        bgfx_destroy_indirect_buffer(gs->indirect_buf);
        gs->indirect_buf.idx = UINT16_MAX;
        gs->indirect_cap = 0;
    }
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
    gs->u_cull_planes.idx      = UINT16_MAX;
    gs->u_cull_params.idx      = UINT16_MAX;
    gs->u_cull_reset_params.idx = UINT16_MAX;
    gs->u_indirect_params.idx  = UINT16_MAX;

    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !(caps->supported & BGFX_CAPS_COMPUTE)) {
        LOG_WARN(LOG_TAG, "GPU has no compute support; GPU-driven path disabled");
        return gs;  /* no-op mode */
    }

    const char *sfx = backend_suffix();
    if (!sfx) {
        LOG_WARN(LOG_TAG, "no shader suffix for current renderer; GPU-driven off");
        return gs;
    }

    /* Always load the 1:1 fallback cull program (used when indirect is absent). */
    gs->cull_program = load_compute(pak, "cs_cull_frustum", sfx);
    if (gs->cull_program.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "cs_cull_frustum load failed; GPU-driven path disabled");
        return gs;
    }

    /* scene_buf layout: 7 vec4 (TEXCOORD0..6).  Used to alloc the transient VB
     * that holds the scene records; stride 112 B (== 7 vec4, so the transient
     * allocator's stride-aligned offset is always 16-byte aligned). */
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

    /* counter_buf layout: one uint per run. */
    bgfx_vertex_layout_begin(&gs->counter_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&gs->counter_layout, BGFX_ATTRIB_TEXCOORD0, 1, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&gs->counter_layout);

    /* runmeta_buf layout: one uvec4 per run (carried as 4 floats; the shader
     * reads it as uvec4 via the buffer's uint format). */
    bgfx_vertex_layout_begin(&gs->runmeta_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&gs->runmeta_layout, BGFX_ATTRIB_TEXCOORD0, 4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&gs->runmeta_layout);

    gs->u_cull_planes = bgfx_create_uniform("u_cull_planes", BGFX_UNIFORM_TYPE_VEC4, 6);
    gs->u_cull_params = bgfx_create_uniform("u_cull_params", BGFX_UNIFORM_TYPE_VEC4, 1);

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
            gs->u_cull_reset_params = bgfx_create_uniform("u_cull_reset_params", BGFX_UNIFORM_TYPE_VEC4, 1);
            gs->u_indirect_params   = bgfx_create_uniform("u_indirect_params",   BGFX_UNIFORM_TYPE_VEC4, 1);
            gs->indirect = true;
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
    if (gs->cull_program.idx    != UINT16_MAX) bgfx_destroy_program(gs->cull_program);
    if (gs->reset_program.idx   != UINT16_MAX) bgfx_destroy_program(gs->reset_program);
    if (gs->compact_program.idx != UINT16_MAX) bgfx_destroy_program(gs->compact_program);
    if (gs->build_program.idx   != UINT16_MAX) bgfx_destroy_program(gs->build_program);
    destroy_visible_buffer(gs);
    if (gs->counter_buf.idx  != UINT16_MAX) bgfx_destroy_dynamic_vertex_buffer(gs->counter_buf);
    if (gs->indirect_buf.idx != UINT16_MAX) bgfx_destroy_indirect_buffer(gs->indirect_buf);
    if (gs->u_cull_planes.idx       != UINT16_MAX) bgfx_destroy_uniform(gs->u_cull_planes);
    if (gs->u_cull_params.idx       != UINT16_MAX) bgfx_destroy_uniform(gs->u_cull_params);
    if (gs->u_cull_reset_params.idx != UINT16_MAX) bgfx_destroy_uniform(gs->u_cull_reset_params);
    if (gs->u_indirect_params.idx   != UINT16_MAX) bgfx_destroy_uniform(gs->u_indirect_params);
    JCE_FREE(gs->rec);
    JCE_FREE(gs->runmeta);
    gs->alloc.free(gs, gs->alloc.ctx);
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

/* ── per-frame batch ──────────────────────────────────────────────────── */

void jce_gpu_scene_begin(JceGpuScene *gs)
{
    if (!gs) return;
    gs->rec_count = 0;
    gs->run_count = 0;
    gs->indirect_ready = false;  /* no valid indirect args until pass-3 runs */
}

bool jce_gpu_scene_add_run(JceGpuScene *gs, const JceGpuSceneRecord *records,
                           uint32_t count, uint32_t num_indices,
                           JceGpuSceneRun *out_run)
{
    if (!gs || !gs->supported || !records || count == 0) return false;

    uint32_t base      = gs->rec_count;
    uint32_t run_index = gs->run_count;

    if (gs->rec_count + count > gs->rec_cap) {
        uint32_t nc = gs->rec_cap ? gs->rec_cap : 1024u;
        while (nc < gs->rec_count + count) nc *= 2u;
        JceGpuSceneRecord *nr = (JceGpuSceneRecord *)JCE_REALLOC(
            gs->rec, (size_t)nc * sizeof(JceGpuSceneRecord));
        if (!nr) return false;
        gs->rec = nr;
        gs->rec_cap = nc;
    }

    /* Grow per-run meta scratch (indirect path) — one slot per run. */
    if (gs->indirect && run_index >= gs->runmeta_cap) {
        uint32_t nc = gs->runmeta_cap ? gs->runmeta_cap * 2u : 256u;
        void *nm = JCE_REALLOC(gs->runmeta, (size_t)nc * sizeof(*gs->runmeta));
        if (!nm) return false;
        gs->runmeta = nm;
        gs->runmeta_cap = nc;
    }

    /* Copy the run's records, tagging each with its partition base + run index so
     * the compact cull appends survivors densely within [base, base+count) and
     * tallies b_counter[run_index].  In the 1:1 fallback the shader ignores these
     * (slot == record id), so they are harmless there. */
    for (uint32_t i = 0; i < count; i++) {
        JceGpuSceneRecord r = records[i];
        r.run_base  = (float)base;
        r.run_index = (float)run_index;
        gs->rec[base + i] = r;
    }
    gs->rec_count += count;

    if (gs->indirect) {
        gs->runmeta[run_index].num_indices = num_indices;
        gs->runmeta[run_index].run_base    = base;
    }
    gs->run_count += 1;

    if (out_run) {
        out_run->run_base    = base;
        out_run->indirect_el = gs->indirect ? run_index : UINT32_MAX;
    }
    return true;
}

/* ── dispatch ─────────────────────────────────────────────────────────── */

/* Upload the whole batch's scene records into a TRANSIENT vertex buffer and bind
 * it as the compute SRV.  Returns false (caller falls back to CPU) on transient-
 * ring exhaustion / short grant.  On success writes *out_handle and *out_off_vec4
 * (the alloc's vec4 offset in the ring, for the shader's record rebasing). */
static bool upload_scene_records(JceGpuScene *gs,
                                 bgfx_vertex_buffer_handle_t *out_handle,
                                 float *out_off_vec4)
{
    if (bgfx_get_avail_transient_vertex_buffer(gs->rec_count, &gs->scene_layout)
        < gs->rec_count) {
        return false;
    }
    bgfx_transient_vertex_buffer_t tvb;
    bgfx_alloc_transient_vertex_buffer(&tvb, gs->rec_count, &gs->scene_layout);
    if (tvb.data == NULL || tvb.handle.idx == UINT16_MAX) return false;

    /* getAvail and alloc are two SEPARATE locked calls against the SHARED
     * transient pool; stride-alignment rounding can grant fewer bytes than
     * reported when the pool is near-full.  Bail if the grant is short. */
    const size_t need_bytes = (size_t)gs->rec_count * SCENE_STRIDE_BYTES;
    if ((size_t)tvb.size < need_bytes) return false;
    memcpy(tvb.data, gs->rec, need_bytes);

    *out_handle   = tvb.handle;
    *out_off_vec4 = (float)((uint64_t)tvb.startVertex * SCENE_VEC4_PER_REC);
    return true;
}

bool jce_gpu_scene_dispatch(JceGpuScene *gs, uint16_t reset_view,
                            uint16_t cull_view, const jce_vec4 planes[6])
{
    if (!gs || !gs->supported || !planes || gs->rec_count == 0) return false;

    JCE_PROFILE_ZONE_N("GpuScene::Dispatch");

    /* No valid indirect args until pass-3 runs this dispatch (defensive: also
     * cleared in begin(), but a fall-through to the 1:1 cull below must leave the
     * draw side seeing an INVALID indirect buffer). */
    gs->indirect_ready = false;

    if (!ensure_capacity(gs, gs->rec_count)) {
        JCE_PROFILE_ZONE_END;
        return false;
    }

    bgfx_vertex_buffer_handle_t scene_h;
    float scene_off_vec4;
    if (!upload_scene_records(gs, &scene_h, &scene_off_vec4)) {
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
        /* Indirect needs the per-run counter + indirect buffers + the run-meta
         * transient.  If any prerequisite fails, fall through to the 1:1 path
         * (still correct, just no compaction) by clearing gs->indirect for THIS
         * dispatch via a local flag. */
        bool ok = ensure_counter_capacity(gs, gs->run_count)
               && ensure_indirect_capacity(gs, gs->run_count);

        bgfx_transient_vertex_buffer_t rmvb;
        float runmeta_off_vec4 = 0.0f;
        if (ok) {
            if (bgfx_get_avail_transient_vertex_buffer(gs->run_count, &gs->runmeta_layout)
                < gs->run_count) {
                ok = false;
            } else {
                bgfx_alloc_transient_vertex_buffer(&rmvb, gs->run_count, &gs->runmeta_layout);
                if (rmvb.data == NULL || rmvb.handle.idx == UINT16_MAX
                    || (size_t)rmvb.size < (size_t)gs->run_count * RUNMETA_STRIDE_BYTES) {
                    ok = false;
                } else {
                    /* Pack run meta as vec4 FLOAT values (numIndices, run_base,
                     * 0, 0): bgfx exposes a vertex buffer's compute SRV as
                     * RGBA32F, so the shader reads vec4 and uint()'s it (same as
                     * the scene-record ids).  Counts/bases are < 2^24 in any real
                     * scene, so the float round-trip is exact. */
                    float *dst = (float *)rmvb.data;
                    for (uint32_t r = 0; r < gs->run_count; r++) {
                        dst[r * 4 + 0] = (float)gs->runmeta[r].num_indices;
                        dst[r * 4 + 1] = (float)gs->runmeta[r].run_base;
                        dst[r * 4 + 2] = 0.0f;
                        dst[r * 4 + 3] = 0.0f;
                    }
                    runmeta_off_vec4 = (float)((uint64_t)rmvb.startVertex);
                }
            }
        }

        if (ok) {
            /* Pass 1: reset per-run counters.  Dispatched on a SEPARATE, earlier
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
                float rparams[4] = { (float)gs->run_count, 0.0f, 0.0f, 0.0f };
                bgfx_set_uniform(gs->u_cull_reset_params, rparams, 1);
                bgfx_set_compute_dynamic_vertex_buffer(0, gs->counter_buf, BGFX_ACCESS_WRITE);
                uint32_t groups = (gs->run_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
                bgfx_dispatch(reset_view, gs->reset_program, groups, 1, 1, BGFX_DISCARD_ALL);
            }
            /* Pass 2: cull + compact survivors into their run partitions. */
            {
                float params[4] = { (float)gs->rec_count, (float)gs->capacity,
                                    scene_off_vec4, 0.0f };
                bgfx_set_uniform(gs->u_cull_planes, pl, 6);
                bgfx_set_uniform(gs->u_cull_params, params, 1);
                bgfx_set_compute_vertex_buffer(0, scene_h, BGFX_ACCESS_READ);
                bgfx_set_compute_dynamic_vertex_buffer(1, gs->visible_buf, BGFX_ACCESS_WRITE);
                bgfx_set_compute_dynamic_vertex_buffer(2, gs->counter_buf, BGFX_ACCESS_READWRITE);
                uint32_t groups = (gs->rec_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
                bgfx_dispatch(cull_view, gs->compact_program, groups, 1, 1, BGFX_DISCARD_ALL);
            }
            /* Pass 3: build the per-run indirect draw args. */
            {
                float iparams[4] = { (float)gs->run_count, runmeta_off_vec4, 0.0f, 0.0f };
                bgfx_set_uniform(gs->u_indirect_params, iparams, 1);
                bgfx_set_compute_vertex_buffer(0, rmvb.handle, BGFX_ACCESS_READ);
                bgfx_set_compute_dynamic_vertex_buffer(1, gs->counter_buf, BGFX_ACCESS_READ);
                bgfx_set_compute_indirect_buffer(2, gs->indirect_buf, BGFX_ACCESS_WRITE);
                uint32_t groups = (gs->run_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
                bgfx_dispatch(cull_view, gs->build_program, groups, 1, 1, BGFX_DISCARD_ALL);
            }
            /* Pass-3 ran: the indirect buffer now holds valid per-run args, so the
             * draw side may consume it via jce_gpu_scene_indirect_buffer(). */
            gs->indirect_ready = true;
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
        bgfx_set_uniform(gs->u_cull_planes, pl, 6);
        bgfx_set_uniform(gs->u_cull_params, params, 1);
        bgfx_set_compute_vertex_buffer(0, scene_h, BGFX_ACCESS_READ);
        bgfx_set_compute_dynamic_vertex_buffer(1, gs->visible_buf, BGFX_ACCESS_WRITE);
        uint32_t groups = (gs->rec_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
        bgfx_dispatch(cull_view, gs->cull_program, groups, 1, 1, BGFX_DISCARD_ALL);
    }

    JCE_PROFILE_ZONE_END;
    return true;
}
