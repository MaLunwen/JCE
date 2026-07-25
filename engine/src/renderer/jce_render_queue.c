/*
 * jce_render_queue.c  Sorted draw-call submission queue with auto-instancing.
 *
 * Collects JceDrawCmd entries, sorts them by the requested mode, and
 * flushes to bgfx.  When sorted with JCE_SORT_BY_MATERIAL or
 * JCE_SORT_FOR_INSTANCING, consecutive entries that share the same
 * (view+program+vbh+ibh+material+index_count) tuple are merged into a
 * single instanced submit (4x4 transform packed per instance).
 *
 * Callers wanting full control can use jce_rq_push_instanced() to provide
 * a pre-built per-instance buffer (e.g. transform + tint colour).
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_render_queue.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <stdlib.h>
#include <string.h>
#include <jce/os/core/jce_thread.h>
#include "renderer/jce_render_encoder.h"
#include <jce/renderer/jce_render_pipeline.h>   /* settings S3: perf tri-state */

#define LOG_TAG "render_queue"

#define INITIAL_CAP_DEFAULT 256

/* ── Internal entry ───────────────────────────────────────────────── */

typedef struct {
    JceDrawCmd cmd;
    /* Explicit-instancing payload (NULL = simple, batchable entry). */
    void    *inst_data;          /* owned copy, freed on clear/destroy */
    uint32_t inst_count;
    uint16_t inst_stride_vec4;
    uint16_t inst_persist_vb;    /* dynamic-VB idx+1 (0 = transient from inst_data) */
} JceRqEntry;

struct JceRenderQueue {
    JceRqEntry *entries;
    uint32_t    count;
    uint32_t    capacity;
    JceRenderQueueStats last_stats;
    JceRqBindMaterialFn bind_material;
    void               *bind_material_user;
    bool                no_batch;   /* transparent queues disable instancing */
};

/* ── Sort comparators ─────────────────────────────────────────────── */

static int cmp_front_to_back(const void *a, const void *b)
{
    const JceDrawCmd *ca = &((const JceRqEntry *)a)->cmd;
    const JceDrawCmd *cb = &((const JceRqEntry *)b)->cmd;
    if (ca->depth < cb->depth) return -1;
    if (ca->depth > cb->depth) return  1;
    if (ca->material_key < cb->material_key) return -1;
    if (ca->material_key > cb->material_key) return  1;
    return 0;
}

static int cmp_back_to_front(const void *a, const void *b)
{
    const JceDrawCmd *ca = &((const JceRqEntry *)a)->cmd;
    const JceDrawCmd *cb = &((const JceRqEntry *)b)->cmd;
    if (ca->depth > cb->depth) return -1;
    if (ca->depth < cb->depth) return  1;
    return 0;
}

static int cmp_by_material(const void *a, const void *b)
{
    const JceDrawCmd *ca = &((const JceRqEntry *)a)->cmd;
    const JceDrawCmd *cb = &((const JceRqEntry *)b)->cmd;
    if (ca->material_key < cb->material_key) return -1;
    if (ca->material_key > cb->material_key) return  1;
    if (ca->depth < cb->depth) return -1;
    if (ca->depth > cb->depth) return  1;
    return 0;
}

static int cmp_for_instancing(const void *a, const void *b)
{
    const JceDrawCmd *ca = &((const JceRqEntry *)a)->cmd;
    const JceDrawCmd *cb = &((const JceRqEntry *)b)->cmd;
    if (ca->view_id      != cb->view_id)      return ca->view_id      < cb->view_id      ? -1 : 1;
    if (ca->program      != cb->program)      return ca->program      < cb->program      ? -1 : 1;
    if (ca->mesh_vbh     != cb->mesh_vbh)     return ca->mesh_vbh     < cb->mesh_vbh     ? -1 : 1;
    if (ca->mesh_ibh     != cb->mesh_ibh)     return ca->mesh_ibh     < cb->mesh_ibh     ? -1 : 1;
    if (ca->index_count  != cb->index_count)  return ca->index_count  < cb->index_count  ? -1 : 1;
    if (ca->material_key != cb->material_key) return ca->material_key < cb->material_key ? -1 : 1;
    if (ca->depth        <  cb->depth)        return -1;
    if (ca->depth        >  cb->depth)        return  1;
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

JceRenderQueue *jce_rq_create(uint32_t initial_capacity)
{
    if (initial_capacity == 0) initial_capacity = INITIAL_CAP_DEFAULT;

    JceRenderQueue *rq = (JceRenderQueue *)JCE_CALLOC(1, sizeof(*rq));
    if (!rq) return NULL;

    rq->entries = (JceRqEntry *)JCE_CALLOC(initial_capacity, sizeof(JceRqEntry));
    if (!rq->entries) {
        JCE_FREE(rq);
        return NULL;
    }
    rq->capacity = initial_capacity;
    rq->count    = 0;
    return rq;
}

static void rq_free_owned_inst(JceRenderQueue *rq)
{
    for (uint32_t i = 0; i < rq->count; i++) {
        if (rq->entries[i].inst_data) {
            JCE_FREE(rq->entries[i].inst_data);
            rq->entries[i].inst_data = NULL;
        }
    }
}

void jce_rq_set_material_binder(JceRenderQueue *rq,
                                 JceRqBindMaterialFn fn, void *user)
{
    if (!rq) return;
    rq->bind_material      = fn;
    rq->bind_material_user = user;
}

void jce_rq_set_no_batch(JceRenderQueue *rq, bool no_batch)
{
    if (!rq) return;
    rq->no_batch = no_batch;
}

void jce_rq_destroy(JceRenderQueue *rq)
{
    if (!rq) return;
    rq_free_owned_inst(rq);
    JCE_FREE(rq->entries);
    JCE_FREE(rq);
}

/* ── Submission ───────────────────────────────────────────────────── */

static bool rq_grow_if_needed(JceRenderQueue *rq)
{
    if (rq->count < rq->capacity) return true;
    uint32_t new_cap = rq->capacity * 2;
    JceRqEntry *new_buf = (JceRqEntry *)JCE_CALLOC(new_cap, sizeof(JceRqEntry));
    if (!new_buf) {
        LOG_WARN(LOG_TAG, "failed to grow queue (cap=%u)", rq->capacity);
        return false;
    }
    memcpy(new_buf, rq->entries, rq->count * sizeof(JceRqEntry));
    JCE_FREE(rq->entries);
    rq->entries  = new_buf;
    rq->capacity = new_cap;
    return true;
}

void jce_rq_push(JceRenderQueue *rq, const JceDrawCmd *cmd)
{
    if (!rq || !cmd) return;
    if (!rq_grow_if_needed(rq)) return;
    JceRqEntry *e = &rq->entries[rq->count++];
    e->cmd = *cmd;
    /* Backwards-compat: zero-initialised cmd → no single-variant program.
     * Callers that want n=1 fallback should set program_single to a real
     * non-instance program handle. */
    if (e->cmd.program_single == 0 && e->cmd.program != 0) {
        e->cmd.program_single = UINT16_MAX;
    }
    e->inst_data = NULL;
    e->inst_count = 0;
    e->inst_stride_vec4 = 0;
    e->inst_persist_vb = 0;
}

void jce_rq_push_instanced(JceRenderQueue *rq, const JceDrawCmd *cmd,
                            const JceInstanceBatch *batch)
{
    if (!rq || !cmd || !batch || batch->instance_count == 0
        || batch->stride_vec4 < 4 || (!batch->data && !batch->persist_vb)) {
        return;
    }
    if (!rq_grow_if_needed(rq)) return;

    /* Persistent-VB path (千万 S1): the instance data already lives in a GPU
     * dynamic vertex buffer — store the handle, NO per-frame CPU copy. */
    void *copy = NULL;
    if (!batch->persist_vb) {
        size_t bytes = (size_t)batch->instance_count * batch->stride_vec4 * 16u;
        copy = JCE_MALLOC(bytes);
        if (!copy) {
            LOG_WARN(LOG_TAG, "failed to copy instance buffer (%zu B)", bytes);
            return;
        }
        memcpy(copy, batch->data, bytes);
    }

    JceRqEntry *e = &rq->entries[rq->count++];
    e->cmd = *cmd;
    e->inst_data = copy;
    e->inst_count = batch->instance_count;
    e->inst_stride_vec4 = batch->stride_vec4;
    e->inst_persist_vb = batch->persist_vb;
}

static bool rq_can_batch(const JceRqEntry *a, const JceRqEntry *b); /* rank-4 early-out */

void jce_rq_sort(JceRenderQueue *rq, JceSortMode mode)
{
    JCE_PROFILE_ZONE_N("RenderQueue::Sort");
    if (!rq || rq->count < 2) { JCE_PROFILE_ZONE_END; return; }
    /* Own perf phase (accumulates across the frame's queues): the sort was
     * never timed apart from flush+submit, so whether the O(N log N) qsort
     * over ~120-byte entries is worth a radix-key rework is unmeasured —
     * this phase provides the number that decides it. */
    uint64_t _t0_sort = jce_time_perf_counter();

    switch (mode) {
    case JCE_SORT_FRONT_TO_BACK:
        qsort(rq->entries, rq->count, sizeof(JceRqEntry), cmp_front_to_back);
        break;
    case JCE_SORT_BACK_TO_FRONT:
        qsort(rq->entries, rq->count, sizeof(JceRqEntry), cmp_back_to_front);
        break;
    case JCE_SORT_BY_MATERIAL:
        qsort(rq->entries, rq->count, sizeof(JceRqEntry), cmp_by_material);
        break;
    case JCE_SORT_FOR_INSTANCING: {
        /* rank-4: skip the O(N log N) qsort when the queue is ALREADY a single
         * instancing batch — every entry shares the batch key and none carries a
         * pre-built instance buffer.  The flush run-detector groups identical
         * consecutive entries without needing them sorted, and instances within a
         * batch are order-independent, so an already-uniform queue flushes
         * byte-identically unsorted.  O(N) precheck vs O(N log N) sort; for a
         * genuine multi-material queue it falls through to the full sort. */
        bool single = true;
        for (uint32_t i = 1; i < rq->count; i++) {
            if (!rq_can_batch(&rq->entries[0], &rq->entries[i])) { single = false; break; }
        }
        if (!single)
            qsort(rq->entries, rq->count, sizeof(JceRqEntry), cmp_for_instancing);
        break;
    }
    case JCE_SORT_SEQUENTIAL:
        break;
    }
    jce_perf_phase_add("rq_sort", jce_time_perf_to_ms(_t0_sort,
                                                      jce_time_perf_counter()));
    JCE_PROFILE_ZONE_END;
}

static bool rq_can_batch(const JceRqEntry *a, const JceRqEntry *b)
{
    if (a->inst_data || b->inst_data) return false;
    const JceDrawCmd *ca = &a->cmd, *cb = &b->cmd;
    return ca->view_id      == cb->view_id
        && ca->program      == cb->program
        && ca->mesh_vbh     == cb->mesh_vbh
        && ca->mesh_ibh     == cb->mesh_ibh
        && ca->index_count  == cb->index_count
        && ca->material_key == cb->material_key;
}

static uint32_t rq_depth_key(float d)
{
    uint32_t k;
    memcpy(&k, &d, sizeof(k));
    if (k & 0x80000000u) k = ~k;
    else                 k |= 0x80000000u;
    return k;
}

/* Resolve the bgfx render state for an entry.  cmd.state==0 keeps the
 * historical behaviour (opaque BGFX_STATE_DEFAULT: cull CW, depth test +
 * write); a non-zero state carries the material's blend / cull / write
 * flags assembled by the caller (e.g. alpha-blend transparent draws). */
static uint64_t rq_resolve_state(const JceDrawCmd *c)
{
    return c->state ? c->state : BGFX_STATE_DEFAULT;
}

static void rq_submit_single(const JceRqEntry *e)
{
    const JceDrawCmd *c = &e->cmd;
    jce_enc_set_transform(c->transform.raw[0], 1);

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)c->mesh_vbh };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)c->mesh_ibh };
    jce_enc_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    /* Non-indexed mesh: skip the bind (see rq_submit_explicit_instanced). */
    if (ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(ibh, 0, c->index_count);
    jce_enc_set_state(rq_resolve_state(c), 0);

    /* For n=1, prefer the non-instance program variant: avoids feeding a
     * 1-element instance buffer to a shader that declares per-instance
     * inputs (D3D12 PSO creation rejects this in some configurations).
     * Caller may pass program_single=0xFFFF if no non-instance variant
     * exists, in which case we fall back to the instance program +
     * 1-element instance buffer. */
    uint16_t prog_idx = (c->program_single != UINT16_MAX) ? c->program_single
                                                          : c->program;

    if (c->program_single == UINT16_MAX) {
        const uint16_t stride_bytes = 64;
        if (bgfx_get_avail_instance_data_buffer(1, stride_bytes) >= 1) {
            bgfx_instance_data_buffer_t idb;
            bgfx_alloc_instance_data_buffer(&idb, 1, stride_bytes);
            /* alloc clamps idb.num to remaining transient space — under the
             * opt-in parallel path two workers can race the avail-check/alloc
             * gap, so re-read before the copy (serial path: idb.num==1 always). */
            if (idb.num >= 1) {
                memcpy(idb.data, c->transform.raw[0], stride_bytes);
                jce_enc_set_instance_data_buffer(&idb, 0, 1);
            }
        }
    }

    bgfx_program_handle_t prog = { prog_idx };
    jce_enc_submit(c->view_id, prog, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
}

static void rq_submit_explicit_instanced(const JceRqEntry *e)
{
    const JceDrawCmd *c = &e->cmd;
    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)c->mesh_vbh };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)c->mesh_ibh };
    jce_enc_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    /* Non-indexed mesh (e.g. a glTF skinned prim without indices): bgfx's
     * convention is to simply not set an index buffer — passing the invalid
     * handle into the encoder is UB that AVs under re-entrant renders. */
    if (ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(ibh, 0, c->index_count);

    if (e->inst_persist_vb) {
        /* 千万 S1: instance data is GPU-resident — bind it directly, no copy. */
        bgfx_dynamic_vertex_buffer_handle_t dvb = { (uint16_t)(e->inst_persist_vb - 1u) };
        jce_enc_set_instance_data_from_dynamic_vertex_buffer(dvb, 0, e->inst_count);
    } else {
        uint16_t stride_b = (uint16_t)(e->inst_stride_vec4 * 16u);
        if (bgfx_get_avail_instance_data_buffer(e->inst_count, stride_b) < e->inst_count)
            return;
        bgfx_instance_data_buffer_t idb;
        bgfx_alloc_instance_data_buffer(&idb, e->inst_count, stride_b);
        /* re-read idb.num: alloc clamps to remaining transient space, and on the
         * opt-in parallel path the avail-check/alloc gap is unsynchronized
         * (serial path: idb.num==e->inst_count always → byte-identical). */
        uint32_t got = idb.num < e->inst_count ? idb.num : e->inst_count;
        memcpy(idb.data, e->inst_data, (size_t)got * stride_b);
        jce_enc_set_instance_data_buffer(&idb, 0, got);
    }
    jce_enc_set_state(rq_resolve_state(c), 0);

    bgfx_program_handle_t prog = { c->program };
    jce_enc_submit(c->view_id, prog, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
}

static bool rq_submit_auto_batched(const JceRqEntry *entries,
                                    uint32_t start, uint32_t end)
{
    uint32_t n = end - start;
    const uint16_t stride_bytes = 64;  /* 4 vec4 = 4x4 matrix */
    if (bgfx_get_avail_instance_data_buffer(n, stride_bytes) < n) {
        return false;
    }
    bgfx_instance_data_buffer_t idb;
    bgfx_alloc_instance_data_buffer(&idb, n, stride_bytes);
    /* re-read idb.num: alloc clamps to remaining transient space, and on the
     * opt-in parallel path the avail-check/alloc gap is unsynchronized — copy
     * only what was allocated (serial path: idb.num==n → byte-identical). */
    uint32_t got = idb.num < n ? idb.num : n;
    uint8_t *dst = idb.data;
    for (uint32_t i = start; i < start + got; i++) {
        memcpy(dst, entries[i].cmd.transform.raw[0], stride_bytes);
        dst += stride_bytes;
    }

    const JceDrawCmd *c = &entries[start].cmd;
    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)c->mesh_vbh };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)c->mesh_ibh };
    jce_enc_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    /* Non-indexed mesh: skip the bind (see rq_submit_explicit_instanced). */
    if (ibh.idx != UINT16_MAX)
        jce_enc_set_index_buffer(ibh, 0, c->index_count);
    jce_enc_set_instance_data_buffer(&idb, 0, got);
    jce_enc_set_state(rq_resolve_state(c), 0);

    bgfx_program_handle_t prog = { c->program };
    jce_enc_submit(c->view_id, prog, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
    return true;
}

/* Flush entries [start, end): bind material + submit, auto-batching runs bounded
 * by `end` (a run straddling a chunk boundary splits into two instanced submits
 * — same pixels). bgfx calls route through the encoder shim. */
static void rq_flush_range(JceRenderQueue *rq, uint32_t start, uint32_t end,
                           bool disable_instancing, bool have_binder,
                           JceRenderQueueStats *st)
{
    uint32_t i = start;
    while (i < end) {
        JceRqEntry *e = &rq->entries[i];
        if (have_binder)
            rq->bind_material(e->cmd.material_key, rq->bind_material_user);
        if (e->inst_persist_vb) {
            rq_submit_explicit_instanced(e); st->submits_out++; i++; continue;
        }
        if (e->inst_data && !disable_instancing) {
            rq_submit_explicit_instanced(e); st->submits_out++; i++; continue;
        }
        if (e->inst_data && disable_instancing) {
            rq_submit_single(e); st->submits_out++; i++; continue;
        }
        uint32_t j = i + 1;
        while (j < end && rq_can_batch(e, &rq->entries[j])) j++;
        uint32_t run = j - i;
        if (!disable_instancing && run >= 2 &&
            rq_submit_auto_batched(rq->entries, i, j)) {
            st->submits_out++; st->batches_merged++; st->instances_total += run;
        } else {
            for (uint32_t k = i; k < j; k++) {
                if (have_binder && k > i)
                    rq->bind_material(rq->entries[k].cmd.material_key, rq->bind_material_user);
                rq_submit_single(&rq->entries[k]); st->submits_out++;
            }
        }
        i = j;
    }
}

/* ── Parallel submit (lever ①, opt-in JCE_PARALLEL_SUBMIT) ──────────────────
 * Split the sorted queue into up to RQ_MAX_CHUNKS ranges; each worker records
 * its range into its OWN bgfx encoder. The encoder pool is shared across ALL
 * flushes in a frame, so it can be exhausted (bgfx_encoder_begin returns NULL)
 * when several queues flush; such a chunk is DEFERRED — recorded and replayed
 * serially on the API thread after the parallel region (the implicit path is
 * only ever used on the API thread, never on a worker — using it on a worker
 * races bgfx's main encoder m_encoder[0], which was the crash). */
#define RQ_MAX_CHUNKS 8
#define RQ_MT_MIN     64

typedef struct {
    JceRenderQueue     *rq;
    bool                disable_instancing;
    bool                have_binder;
    uint32_t            per;
    uint64_t            api_tid;
    JceRenderQueueStats stats[RQ_MAX_CHUNKS];
    uint32_t            defer_begin[RQ_MAX_CHUNKS];   /* ranges that got no encoder */
    uint32_t            defer_end[RQ_MAX_CHUNKS];
} RqFlushCtx;

static void rq_flush_chunk(uint32_t begin, uint32_t end, void *user)
{
    RqFlushCtx *c = (RqFlushCtx *)user;
    /* Exact: jce_thread_pool_parallel_for pins `begin` to a multiple of `per`,
     * so each chunk index is visited by exactly one worker — stats[ci] and
     * defer_*[ci] are single-writer slots, not accumulators. */
    uint32_t ci = (c->per ? begin / c->per : 0u);
    if (ci >= RQ_MAX_CHUNKS) ci = RQ_MAX_CHUNKS - 1;

    /* The parallel-for wait drains COOPERATIVELY: the API thread runs one chunk
     * itself — it must use the implicit path (it owns m_encoder[0]). */
    if (jce_thread_current_id() == c->api_tid) {
        jce_render_encoder_set(NULL);
        rq_flush_range(c->rq, begin, end,
                       c->disable_instancing, c->have_binder, &c->stats[ci]);
        return;
    }
    bgfx_encoder_t *enc = bgfx_encoder_begin(true);
    if (!enc) {                       /* pool exhausted → defer to the API thread */
        c->defer_begin[ci] = begin;
        c->defer_end[ci]   = end;
        return;
    }
    jce_render_encoder_set(enc);
    rq_flush_range(c->rq, begin, end,
                   c->disable_instancing, c->have_binder, &c->stats[ci]);
    jce_render_encoder_set(NULL);
    bgfx_encoder_end(enc);
}

void jce_rq_flush(JceRenderQueue *rq, const JceRenderer *renderer)
{
    JCE_PROFILE_ZONE_N("RenderQueue::Flush");
    if (!rq || rq->count == 0) { JCE_PROFILE_ZONE_END; return; }
    (void)renderer;

    static int s_disable_inst_env = -1;
    if (s_disable_inst_env < 0) {
        const char *v = getenv("JCE_RQ_DISABLE_INSTANCING");
        s_disable_inst_env = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    bool disable_instancing = (s_disable_inst_env != 0) || rq->no_batch;

    JceRenderQueueStats st;
    memset(&st, 0, sizeof(st));
    st.commands_in = rq->count;
    bool have_binder = (rq->bind_material != NULL);

    /* Material binds happen before EVERY submit (bgfx records only the uniform
     * updates since the last state-discarding submit and replays them in
     * view-sorted order; BGFX_DISCARD_ALL clears textures), so a submit without
     * its own bind would go out unbound. Batched runs collapse to one bind. */

    static int s_mt_env = -2;
    if (s_mt_env == -2) {
        const char *v = getenv("JCE_PARALLEL_SUBMIT");
        s_mt_env = (!v || !v[0]) ? -1 : (v[0] != '0');
    }
    const bool mt_enabled = (s_mt_env >= 0) ? (s_mt_env != 0)
        : jce_render_pipeline_perf_enabled(JCE_RP_PERF_PARALLEL_SUBMIT, false);
    JceThreadPool *pool = mt_enabled ? jce_thread_pool_shared() : NULL;
    int nworkers = pool ? jce_thread_pool_worker_count(pool) : 0;

    /* !rq->no_batch excludes the order-dependent transparent queue (no_batch +
     * back-to-front): in bgfx SEQUENTIAL view mode the sort key is the atomic
     * per-view seq counter = physical submit order, so splitting it across
     * worker encoders would scramble back-to-front alpha blending. The opaque
     * queue is depth-tested (order-independent), so it parallelizes safely. */
    if (mt_enabled && nworkers > 1 && have_binder && !rq->no_batch && rq->count >= RQ_MT_MIN) {
        /* Warm every per-frame shared cache (light_env repack, baked-GI/fog
         * snapshots, lazy ensure_uniforms) ONCE on the main thread so the worker
         * binds are read-only; bgfx_discard drops the pending state off the
         * implicit encoder so the warmup leaves no draw. */
        rq->bind_material(rq->entries[0].cmd.material_key, rq->bind_material_user);
        bgfx_discard(BGFX_DISCARD_ALL);

        int chunks = nworkers < RQ_MAX_CHUNKS ? nworkers : RQ_MAX_CHUNKS;
        uint32_t per = (rq->count + (uint32_t)chunks - 1u) / (uint32_t)chunks;
        if (per == 0u) per = rq->count;

        RqFlushCtx ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.rq = rq;
        ctx.disable_instancing = disable_instancing;
        ctx.have_binder = have_binder;
        ctx.per = per;
        ctx.api_tid = jce_thread_current_id();
        jce_thread_pool_parallel_for(pool, rq->count, per, rq_flush_chunk, &ctx);

        /* Replay any deferred (pool-exhausted) ranges serially on the API thread
         * via the implicit encoder — safe here, never on a worker. */
        for (int k = 0; k < RQ_MAX_CHUNKS; k++) {
            if (ctx.defer_end[k] > ctx.defer_begin[k]) {
                jce_render_encoder_set(NULL);
                rq_flush_range(rq, ctx.defer_begin[k], ctx.defer_end[k],
                               disable_instancing, have_binder, &ctx.stats[k]);
            }
            st.submits_out     += ctx.stats[k].submits_out;
            st.batches_merged  += ctx.stats[k].batches_merged;
            st.instances_total += ctx.stats[k].instances_total;
        }
    } else {
        rq_flush_range(rq, 0, rq->count, disable_instancing, have_binder, &st);
    }

    rq_free_owned_inst(rq);
    rq->count = 0;
    rq->last_stats = st;
    JCE_PROFILE_ZONE_END;
}

void jce_rq_clear(JceRenderQueue *rq)
{
    if (!rq) return;
    rq_free_owned_inst(rq);
    rq->count = 0;
}

uint32_t jce_rq_count(const JceRenderQueue *rq)
{
    return rq ? rq->count : 0;
}

void jce_rq_last_stats(const JceRenderQueue *rq, JceRenderQueueStats *out)
{
    if (!rq || !out) return;
    *out = rq->last_stats;
}

void jce_rq_analyse(const JceRenderQueue *rq, JceRenderQueueStats *out)
{
    if (!rq || !out) return;
    JceRenderQueueStats st;
    memset(&st, 0, sizeof(st));
    st.commands_in = rq->count;

    uint32_t i = 0;
    while (i < rq->count) {
        const JceRqEntry *e = &rq->entries[i];
        if (e->inst_data || e->inst_persist_vb) {
            st.submits_out++;
            i++;
            continue;
        }
        uint32_t j = i + 1;
        while (j < rq->count && rq_can_batch(e, &rq->entries[j])) j++;
        uint32_t run = j - i;
        if (run >= 2) {
            st.submits_out++;
            st.batches_merged++;
            st.instances_total += run;
        } else {
            st.submits_out++;
        }
        i = j;
    }
    *out = st;
}
