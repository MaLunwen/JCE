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
#include "jce_instance_upload.h"

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
    /* PRIORITY FIRST, then depth.  Depth alone has no answer for two coplanar
     * transparents, and "whichever the camera happens to be nearer" is what
     * makes them flicker as it moves.  A higher priority draws LATER, which is
     * on top -- the same direction as Unity's renderQueue and Godot's
     * render_priority, so an author moving between them is not surprised.
     * Neutral is 0, which is what a zeroed JceDrawCmd already carries. */
    if (ca->priority != cb->priority)
        return (ca->priority < cb->priority) ? -1 : 1;
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
    /* AND THE OFFSET.  Two runs of the SAME mesh share vbh, ibh and often
       index_count, so without this they sort adjacent, compare equal to the
       merge predicate below, and become one instanced batch that draws the
       FIRST run's range twice -- the second member's triangles vanishing and
       the first's appearing twice, from a merge that looked like every other
       correct one. */
    if (ca->first_index  != cb->first_index)  return ca->first_index  < cb->first_index  ? -1 : 1;
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
        || batch->stride_vec4 < 4 || batch->stride_vec4 > UINT16_MAX / 16u
        || batch->instance_count > SIZE_MAX / ((size_t)batch->stride_vec4 * 16u)
        || (!batch->data && !batch->persist_vb)) {
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
        && ca->first_index  == cb->first_index
        && ca->material_key == cb->material_key
        /* The submit issues ONE state and ONE stencil for the whole run, so
         * entries that disagree on either cannot share it.  material_key
         * folds both in already; comparing them here as well is the direct
         * guarantee rather than a property of a hash. */
        && ca->state        == cb->state
        && ca->stencil      == cb->stencil;
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

/* State and stencil travel together: bgfx resets both after each submit, so
 * every place that sets one sets the other.  Skipped when zero because the
 * post-submit reset already leaves the stencil at exactly that value -- this
 * is the hot path, and a no-op call per draw is not free. */
static void rq_apply_state(const JceDrawCmd *c)
{
    jce_enc_set_state(rq_resolve_state(c), 0);
    if (c->stencil) jce_enc_set_stencil(c->stencil, BGFX_STENCIL_NONE);
}

static bool rq_program_valid(uint16_t program)
{
    return jce_shader_deferred_resolve(program) != UINT16_MAX;
}

static void rq_bind_mesh(const JceDrawCmd *c)
{
    bgfx_vertex_buffer_handle_t vb = {(uint16_t)c->mesh_vbh};
    bgfx_index_buffer_handle_t ib = {(uint16_t)c->mesh_ibh};
    jce_enc_set_vertex_buffer(0, vb, 0, UINT32_MAX);
    if (ib.idx != UINT16_MAX)
        jce_enc_set_index_buffer(ib, c->first_index, c->index_count);
    rq_apply_state(c);
}

static bool rq_submit_single(const JceRqEntry *e, JceInstanceUploadOwner *owner)
{
    const JceDrawCmd *c = &e->cmd;
    uint16_t index = c->program_single != UINT16_MAX ? c->program_single : c->program;
    if (!rq_program_valid(index)) return false;
    if (c->program_single == UINT16_MAX) {
        if (owner) {
            JceInstanceUpload upload;
            if (!jce_instance_upload_begin(owner, jce_renderer_instance_upload_epoch(), 1, 64, 128, &upload))
                return false;
            memcpy(upload.data, c->transform.raw[0], 64);
            if (!jce_instance_upload_commit(owner, &upload, 1) ||
                !jce_instance_upload_bind_encoder(owner, &upload, 1, jce_tls_encoder)) {
                jce_instance_upload_discard(owner, &upload);
                return false;
            }
        } else {
            bgfx_instance_data_buffer_t data;
            if (bgfx_get_avail_instance_data_buffer(1, 64) < 1) return false;
            bgfx_alloc_instance_data_buffer(&data, 1, 64);
            if (data.num < 1 || !data.data || data.size < 64) return false;
            memcpy(data.data, c->transform.raw[0], 64);
            jce_enc_set_instance_data_buffer(&data, 0, 1);
        }
    }
    jce_enc_set_transform(c->transform.raw[0], 1);
    rq_bind_mesh(c);
    bgfx_program_handle_t program = {index};
    jce_enc_submit(c->view_id, program, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
    return true;
}

/* Every admitted slice binds its own material/state; BGFX_DISCARD_ALL resets
 * these after each submit. Owner mutation is confined to serial API-thread use. */
static uint32_t rq_submit_instances(JceRenderQueue *rq, const JceRqEntry *e,
    const JceRqEntry *matrices, uint32_t count, JceInstanceUploadOwner *owner,
    JceRenderQueueStats *stats)
{
    const JceDrawCmd *c = &e->cmd;
    if (!rq_program_valid(c->program)) return 0;
    uint16_t stride = matrices ? 64 : (uint16_t)(e->inst_stride_vec4 * 16u);
    uint32_t done = 0;
    do {
        uint32_t got = count - done;
        bgfx_instance_data_buffer_t transient;
        JceInstanceUpload upload;
        bool generic = owner && (stride == 64 || stride == 80);
        uint8_t *data = NULL;
        if (e->inst_persist_vb) {
            got = count;
        } else if (generic) {
            if (!jce_instance_upload_begin(owner, jce_renderer_instance_upload_epoch(), got, stride, 128, &upload)) break;
            got = upload.count;
            data = (uint8_t *)upload.data;
        } else {
            uint32_t available = bgfx_get_avail_instance_data_buffer(got, stride);
            if (!available) break;
            if (available < got) got = available;
            bgfx_alloc_instance_data_buffer(&transient, got, stride);
            if (transient.num < got) got = transient.num;
            if (transient.size / stride < got) got = transient.size / stride;
            if (!got || !transient.data) break;
            data = transient.data;
        }
        if (data) {
            if (matrices) {
                for (uint32_t n = 0; n < got; ++n)
                    memcpy(data + (size_t)n * stride, matrices[done + n].cmd.transform.raw[0], 64);
            } else {
                memcpy(data, (uint8_t *)e->inst_data + (size_t)done * stride, (size_t)got * stride);
            }
        }
        if (generic && !e->inst_persist_vb) {
            if (!jce_instance_upload_commit(owner, &upload, got) ||
                !jce_instance_upload_bind_encoder(owner, &upload, got, jce_tls_encoder)) {
                jce_instance_upload_discard(owner, &upload);
                break;
            }
        } else if (e->inst_persist_vb) {
            bgfx_dynamic_vertex_buffer_handle_t vb = {(uint16_t)(e->inst_persist_vb - 1u)};
            jce_enc_set_instance_data_from_dynamic_vertex_buffer(vb, 0, got);
        } else {
            jce_enc_set_instance_data_buffer(&transient, 0, got);
        }
        if (rq->bind_material) rq->bind_material(c->material_key, rq->bind_material_user);
        rq_bind_mesh(c);
        bgfx_program_handle_t program = {c->program};
        jce_enc_submit(c->view_id, program, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
        ++stats->submits_out;
        if (matrices && got >= 2) ++stats->batches_merged;
        stats->instances_total += got;
        done += got;
    } while (done < count);
    if (done < count)
        LOG_WARN(LOG_TAG, "instance upload incomplete: requested=%u submitted=%u stride=%u", count, done, stride);
    return done;
}

static void rq_flush_range(JceRenderQueue *rq, uint32_t start, uint32_t end,
    bool disable_instancing, bool have_binder, JceRenderQueueStats *stats,
    JceInstanceUploadOwner *owner)
{
    uint32_t i = start;
    while (i < end) {
        JceRqEntry *entry = &rq->entries[i];
        /* no_batch/disable_instancing disable automatic merging, not the
         * authored payload of an explicit batch (including tint attributes). */
        if (entry->inst_persist_vb || entry->inst_data) {
            rq_submit_instances(rq, entry, NULL, entry->inst_count, owner, stats);
            ++i;
            continue;
        }
        uint32_t last = i + 1;
        if (!entry->inst_data)
            while (last < end && rq_can_batch(entry, &rq->entries[last])) ++last;
        uint32_t done = 0;
        if (!disable_instancing && last - i >= 2) {
            done = rq_submit_instances(rq, entry, &rq->entries[i], last - i, owner, stats);
        }
        for (uint32_t n = i + done; n < last; ++n) {
            if (have_binder) rq->bind_material(rq->entries[n].cmd.material_key, rq->bind_material_user);
            if (rq_submit_single(&rq->entries[n], owner)) ++stats->submits_out;
        }
        i = last;
    }
}

/* Reserve-aware conservative admission before worker dispatch. The estimate
 * includes alignment per command, even when sorted runs will share a slice. */
static bool rq_parallel_fits(const JceRenderQueue *rq, const JceInstanceUploadOwner *owner)
{
    uint64_t bytes = owner ? owner->transient_reserve_bytes : 0;
    for (uint32_t i = 0; i < rq->count; ++i) {
        const JceRqEntry *e = &rq->entries[i];
        if (e->inst_persist_vb) continue;
        uint64_t count = e->inst_data ? e->inst_count : 1;
        uint64_t stride = e->inst_data ? (uint64_t)e->inst_stride_vec4 * 16u : 64u;
        bytes += count * stride + 15u;
        if (bytes > UINT32_MAX) return false;
    }
    return bytes <= (uint64_t)bgfx_get_avail_instance_data_buffer(UINT32_MAX / 64u, 64) * 64u;
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
                       c->disable_instancing, c->have_binder, &c->stats[ci], NULL);
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
                   c->disable_instancing, c->have_binder, &c->stats[ci], NULL);
    jce_render_encoder_set(NULL);
    bgfx_encoder_end(enc);
}

void jce_rq_flush(JceRenderQueue *rq, const JceRenderer *renderer)
{
    JCE_PROFILE_ZONE_N("RenderQueue::Flush");
    if (!rq || rq->count == 0) { JCE_PROFILE_ZONE_END; return; }
    JceInstanceUploadOwner *owner = renderer ? jce_renderer_instance_upload_owner(renderer) : NULL;

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
    if (mt_enabled && nworkers > 1 && have_binder && !rq->no_batch && rq->count >= RQ_MT_MIN
        && rq_parallel_fits(rq, owner)) {
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
        jce_thread_pool_parallel_for_named(
            pool, "render-queue.flush", rq->count, per,
            rq_flush_chunk, &ctx);

        /* Replay any deferred (pool-exhausted) ranges serially on the API thread
         * via the implicit encoder — safe here, never on a worker. */
        for (int k = 0; k < RQ_MAX_CHUNKS; k++) {
            if (ctx.defer_end[k] > ctx.defer_begin[k]) {
                jce_render_encoder_set(NULL);
                rq_flush_range(rq, ctx.defer_begin[k], ctx.defer_end[k],
                               disable_instancing, have_binder, &ctx.stats[k], owner);
            }
            st.submits_out     += ctx.stats[k].submits_out;
            st.batches_merged  += ctx.stats[k].batches_merged;
            st.instances_total += ctx.stats[k].instances_total;
        }
    } else {
        rq_flush_range(rq, 0, rq->count, disable_instancing, have_binder, &st, owner);
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
