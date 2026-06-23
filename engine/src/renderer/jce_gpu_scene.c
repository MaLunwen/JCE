/*
 * jce_gpu_scene.c -- GPU-driven rendering: compute frustum cull (roadmap #18,
 * Phase 0+1).  See jce_gpu_scene.h.
 *
 * STAGING-FREE DESIGN (no per-frame bgfx_update_dynamic_* on compute buffers)
 * --------------------------------------------------------------------------
 * The original cut issued THREE per-frame bgfx_update_dynamic_* calls inside
 * dispatch (scene records, visible-buffer zero-clear, per-run counter reset).
 * On the D3D12 backend each one routes through BufferD3D12::update, which calls
 * createCommittedResource(Upload, ...) per call to stage the copy.  Under GPU
 * resource pressure (the editor's dual viewport + ImGui) that allocation can
 * fail and the backend NULL-derefs its result -> render-thread access
 * violation.  This file eliminates ALL THREE:
 *
 *   scene_buf  -> a TRANSIENT vertex buffer.  bgfx already uploads the whole
 *                 transient ring with ONE staging allocation per frame
 *                 (RendererContextD3D12::submit, "Update transient vertex
 *                 buffer"), so piggy-backing the records there costs zero extra
 *                 committed-resource staging allocations.  The transient VB's
 *                 underlying handle is bound to the cull compute stage as a
 *                 read SRV (bgfx builds a vec4 SRV over the whole ring for any
 *                 vertex buffer); the shader rebases each record by the
 *                 allocation's vec4 offset in the ring (u_cull_params.z).
 *   visible_buf-> still a persistent COMPUTE_READ_WRITE dynamic VB, but it is
 *                 NEVER updated from the CPU: the redesigned cs_cull_frustum
 *                 writes EVERY slot 1:1 (record id -> slot id), survivors get
 *                 their world matrix and culled records get a zero matrix
 *                 (degenerate instance).  No pre-clear pass is needed.
 *   counter_buf-> DELETED.  The 1:1 slot mapping needs no atomic compaction
 *                 counter, so there is no per-run counter buffer and no reset.
 *
 * Net result: jce_gpu_scene_dispatch makes ZERO bgfx_update_dynamic_* calls.
 *
 * One per-frame BATCH covers the whole color-pass instanced set, so a single
 * cull dispatch (on the pre-color compute view) produces every slot before any
 * draw runs (bgfx orders the compute view ahead of the color view).  The
 * visible buffer is therefore a flat 1:1 image of the scene records: run R's
 * draw sources the contiguous slice [run_base, run_base+count) where run_base
 * is the record's global index, identical to before.
 *
 * Layout (must mirror cs_cull_frustum.sc):
 *   scene_buf (transient VB) : 7 vec4 per record (stride 112 B), bound RO,
 *                              ring-rebased by u_cull_params.z (vec4 offset).
 *   visible_buf (dynamic VB) : 4 vec4 per slot (a mat4 instance stream),
 *                              COMPUTE_READ_WRITE; slot id == record id.
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

/* The record struct MUST be exactly the 7-vec4 (112 B) GPU layout so a straight
 * memcpy populates the scene buffer.  C99 has no _Static_assert; use the
 * negative-array-size compile-time check idiom. */
typedef char jce_gpu_scene_record_size_check[
    (sizeof(JceGpuSceneRecord) == SCENE_STRIDE_BYTES) ? 1 : -1];

struct JceGpuScene {
    bool            supported;
    jce_allocator_t alloc;

    bgfx_program_handle_t cull_program;

    /* Persistent visible-instance buffer (grown on demand to the largest batch
     * seen).  Written 1:1 by the cull; never CPU-updated. */
    bgfx_dynamic_vertex_buffer_handle_t visible_buf;
    uint32_t        capacity;        /* visible slots the buffer can hold */

    bgfx_vertex_layout_t scene_layout;     /* 7 vec4 (transient scene records) */
    bgfx_vertex_layout_t visible_layout;   /* 4 vec4 (instance mat4)           */

    bgfx_uniform_handle_t u_cull_planes;   /* vec4[6] */
    bgfx_uniform_handle_t u_cull_params;   /* vec4    */

    /* Per-frame batch (host scratch accumulated by add_run, uploaded by
     * dispatch into a transient VB). */
    JceGpuSceneRecord *rec;
    uint32_t           rec_count;
    uint32_t           rec_cap;
    uint32_t           run_count;    /* number of runs appended this frame */
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

/* ── visible-buffer (re)allocation ────────────────────────────────────── */

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

/* ── lifecycle ────────────────────────────────────────────────────────── */

JceGpuScene *jce_gpu_scene_create(const JcePakArchive *pak, jce_allocator_t alloc)
{
    JceGpuScene *gs = (JceGpuScene *)alloc.alloc(sizeof(JceGpuScene), alloc.ctx);
    if (!gs) return NULL;
    memset(gs, 0, sizeof(*gs));
    gs->alloc = alloc;
    gs->cull_program.idx  = UINT16_MAX;
    gs->visible_buf.idx   = UINT16_MAX;
    gs->u_cull_planes.idx = UINT16_MAX;
    gs->u_cull_params.idx = UINT16_MAX;

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

    gs->u_cull_planes = bgfx_create_uniform("u_cull_planes", BGFX_UNIFORM_TYPE_VEC4, 6);
    gs->u_cull_params = bgfx_create_uniform("u_cull_params", BGFX_UNIFORM_TYPE_VEC4, 1);

    gs->supported = true;
    LOG_SUCCESS(LOG_TAG, "GPU-driven scene online (compute cull ready, staging-free)");
    return gs;
}

void jce_gpu_scene_destroy(JceGpuScene *gs)
{
    if (!gs) return;
    if (gs->cull_program.idx != UINT16_MAX) bgfx_destroy_program(gs->cull_program);
    destroy_visible_buffer(gs);
    if (gs->u_cull_planes.idx != UINT16_MAX) bgfx_destroy_uniform(gs->u_cull_planes);
    if (gs->u_cull_params.idx != UINT16_MAX) bgfx_destroy_uniform(gs->u_cull_params);
    JCE_FREE(gs->rec);
    gs->alloc.free(gs, gs->alloc.ctx);
}

bool jce_gpu_scene_is_supported(const JceGpuScene *gs)
{
    return gs && gs->supported;
}

uint16_t jce_gpu_scene_visible_vb(const JceGpuScene *gs)
{
    return (gs && gs->supported) ? gs->visible_buf.idx : UINT16_MAX;
}

/* ── per-frame batch ──────────────────────────────────────────────────── */

void jce_gpu_scene_begin(JceGpuScene *gs)
{
    if (!gs) return;
    gs->rec_count = 0;
    gs->run_count = 0;
}

bool jce_gpu_scene_add_run(JceGpuScene *gs, const JceGpuSceneRecord *records,
                           uint32_t count, uint32_t *out_run_base)
{
    if (!gs || !gs->supported || !records || count == 0) return false;

    uint32_t base = gs->rec_count;

    if (gs->rec_count + count > gs->rec_cap) {
        uint32_t nc = gs->rec_cap ? gs->rec_cap : 1024u;
        while (nc < gs->rec_count + count) nc *= 2u;
        JceGpuSceneRecord *nr = (JceGpuSceneRecord *)JCE_REALLOC(
            gs->rec, (size_t)nc * sizeof(JceGpuSceneRecord));
        if (!nr) return false;
        gs->rec = nr;
        gs->rec_cap = nc;
    }

    /* Copy the run's records.  The 1:1 cull maps record id -> visible slot id,
     * so the run's draw slice is simply [base, base+count); run_base/run_index
     * are no longer consumed by the shader (kept zero for clarity). */
    for (uint32_t i = 0; i < count; i++) {
        JceGpuSceneRecord r = records[i];
        r.run_base  = 0.0f;
        r.run_index = 0.0f;
        gs->rec[base + i] = r;
    }
    gs->rec_count += count;
    gs->run_count += 1;

    if (out_run_base) *out_run_base = base;
    return true;
}

bool jce_gpu_scene_dispatch(JceGpuScene *gs, uint16_t cull_view,
                            const jce_vec4 planes[6])
{
    if (!gs || !gs->supported || !planes || gs->rec_count == 0) return false;

    JCE_PROFILE_ZONE_N("GpuScene::Dispatch");

    if (!ensure_capacity(gs, gs->rec_count)) {
        JCE_PROFILE_ZONE_END;
        return false;
    }

    /* Upload the whole batch's records into a TRANSIENT vertex buffer.  bgfx
     * flushes the entire transient ring with a SINGLE staging allocation per
     * frame, so this adds zero per-call committed-resource staging (the path
     * that NULL-derefs under pressure).  Guard against transient-ring
     * exhaustion: if fewer slots are available than we need, fall back to the
     * CPU path (caller draws every queued run from inst_batch). */
    if (bgfx_get_avail_transient_vertex_buffer(gs->rec_count, &gs->scene_layout)
        < gs->rec_count) {
        JCE_PROFILE_ZONE_END;
        return false;
    }

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_alloc_transient_vertex_buffer(&tvb, gs->rec_count, &gs->scene_layout);
    if (tvb.data == NULL || tvb.handle.idx == UINT16_MAX) {
        JCE_PROFILE_ZONE_END;
        return false;
    }
    memcpy(tvb.data, gs->rec, (size_t)gs->rec_count * SCENE_STRIDE_BYTES);

    /* The compute SRV covers the whole transient ring from element 0 (bgfx
     * builds a vec4 SRV: NumElements = ringSize/16).  Rebase the shader's
     * record indexing by this allocation's vec4 offset.  startVertex is in
     * record units (the alloc offset is stride-aligned to 112 B == 7*16, so
     * it is also 16-B aligned); scene_off = startVertex * 7 vec4. */
    const float scene_off_vec4 = (float)((uint64_t)tvb.startVertex * SCENE_VEC4_PER_REC);

    /* ── cull dispatch (one thread per record, whole batch) ───────────── */
    {
        float pl[24];
        for (int i = 0; i < 6; i++) {
            pl[i * 4 + 0] = planes[i].x;
            pl[i * 4 + 1] = planes[i].y;
            pl[i * 4 + 2] = planes[i].z;
            pl[i * 4 + 3] = planes[i].w;
        }
        float params[4] = { (float)gs->rec_count, (float)gs->capacity,
                            scene_off_vec4, 0.0f };
        bgfx_set_uniform(gs->u_cull_planes, pl, 6);
        bgfx_set_uniform(gs->u_cull_params, params, 1);

        bgfx_set_compute_vertex_buffer(0, tvb.handle, BGFX_ACCESS_READ);
        bgfx_set_compute_dynamic_vertex_buffer(1, gs->visible_buf, BGFX_ACCESS_WRITE);

        uint32_t groups = (gs->rec_count + CULL_THREADS_X - 1u) / CULL_THREADS_X;
        bgfx_dispatch(cull_view, gs->cull_program, groups, 1, 1, BGFX_DISCARD_ALL);
    }

    JCE_PROFILE_ZONE_END;
    return true;
}
