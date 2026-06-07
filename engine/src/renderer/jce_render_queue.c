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
#include <jce/renderer/jce_render_queue.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "render_queue"

#define INITIAL_CAP_DEFAULT 256

/* ── Internal entry ───────────────────────────────────────────────── */

typedef struct {
    JceDrawCmd cmd;
    /* Explicit-instancing payload (NULL = simple, batchable entry). */
    void    *inst_data;          /* owned copy, freed on clear/destroy */
    uint32_t inst_count;
    uint16_t inst_stride_vec4;
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
}

void jce_rq_push_instanced(JceRenderQueue *rq, const JceDrawCmd *cmd,
                            const JceInstanceBatch *batch)
{
    if (!rq || !cmd || !batch || !batch->data || batch->instance_count == 0
        || batch->stride_vec4 < 4) {
        return;
    }
    if (!rq_grow_if_needed(rq)) return;

    size_t bytes = (size_t)batch->instance_count * batch->stride_vec4 * 16u;
    void *copy = JCE_MALLOC(bytes);
    if (!copy) {
        LOG_WARN(LOG_TAG, "failed to copy instance buffer (%zu B)", bytes);
        return;
    }
    memcpy(copy, batch->data, bytes);

    JceRqEntry *e = &rq->entries[rq->count++];
    e->cmd = *cmd;
    e->inst_data = copy;
    e->inst_count = batch->instance_count;
    e->inst_stride_vec4 = batch->stride_vec4;
}

void jce_rq_sort(JceRenderQueue *rq, JceSortMode mode)
{
    JCE_PROFILE_ZONE_N("RenderQueue::Sort");
    if (!rq || rq->count < 2) { JCE_PROFILE_ZONE_END; return; }

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
    case JCE_SORT_FOR_INSTANCING:
        qsort(rq->entries, rq->count, sizeof(JceRqEntry), cmp_for_instancing);
        break;
    case JCE_SORT_SEQUENTIAL:
        break;
    }
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
    bgfx_set_transform(c->transform.raw[0], 1);

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)c->mesh_vbh };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)c->mesh_ibh };
    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    bgfx_set_index_buffer(ibh, 0, c->index_count);
    bgfx_set_state(rq_resolve_state(c), 0);

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
            memcpy(idb.data, c->transform.raw[0], stride_bytes);
            bgfx_set_instance_data_buffer(&idb, 0, 1);
        }
    }

    bgfx_program_handle_t prog = { prog_idx };
    bgfx_submit(c->view_id, prog, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
}

static void rq_submit_explicit_instanced(const JceRqEntry *e)
{
    const JceDrawCmd *c = &e->cmd;
    uint16_t stride_b = (uint16_t)(e->inst_stride_vec4 * 16u);
    if (bgfx_get_avail_instance_data_buffer(e->inst_count, stride_b) < e->inst_count) {
        return;
    }
    bgfx_instance_data_buffer_t idb;
    bgfx_alloc_instance_data_buffer(&idb, e->inst_count, stride_b);
    memcpy(idb.data, e->inst_data, (size_t)e->inst_count * stride_b);

    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)c->mesh_vbh };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)c->mesh_ibh };
    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    bgfx_set_index_buffer(ibh, 0, c->index_count);
    bgfx_set_instance_data_buffer(&idb, 0, e->inst_count);
    bgfx_set_state(rq_resolve_state(c), 0);

    bgfx_program_handle_t prog = { c->program };
    bgfx_submit(c->view_id, prog, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
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
    uint8_t *dst = idb.data;
    for (uint32_t i = start; i < end; i++) {
        memcpy(dst, entries[i].cmd.transform.raw[0], stride_bytes);
        dst += stride_bytes;
    }

    const JceDrawCmd *c = &entries[start].cmd;
    bgfx_vertex_buffer_handle_t vbh = { (uint16_t)c->mesh_vbh };
    bgfx_index_buffer_handle_t  ibh = { (uint16_t)c->mesh_ibh };
    bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
    bgfx_set_index_buffer(ibh, 0, c->index_count);
    bgfx_set_instance_data_buffer(&idb, 0, n);
    bgfx_set_state(rq_resolve_state(c), 0);

    bgfx_program_handle_t prog = { c->program };
    bgfx_submit(c->view_id, prog, rq_depth_key(c->depth), BGFX_DISCARD_ALL);
    return true;
}

void jce_rq_flush(JceRenderQueue *rq, const JceRenderer *renderer)
{
    JCE_PROFILE_ZONE_N("RenderQueue::Flush");
    if (!rq || rq->count == 0) { JCE_PROFILE_ZONE_END; return; }
    (void)renderer;

    /* JCE_RQ_DISABLE_INSTANCING=1 forces the per-entry single submit
     * path (uses the non-instance program); useful as a kill-switch
     * if a backend ever rejects the instanced PSO again. */
    static int s_disable_inst_env = -1;
    if (s_disable_inst_env < 0) {
        const char *v = getenv("JCE_RQ_DISABLE_INSTANCING");
        s_disable_inst_env = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    bool disable_instancing = (s_disable_inst_env != 0) || rq->no_batch;

    JceRenderQueueStats st;
    memset(&st, 0, sizeof(st));
    st.commands_in = rq->count;

    bool     have_binder       = (rq->bind_material != NULL);
    bool     bound_any         = false;
    uint32_t last_material_key = 0;

    uint32_t i = 0;
    while (i < rq->count) {
        JceRqEntry *e = &rq->entries[i];

        if (have_binder &&
            (!bound_any || e->cmd.material_key != last_material_key)) {
            rq->bind_material(e->cmd.material_key, rq->bind_material_user);
            last_material_key = e->cmd.material_key;
            bound_any = true;
        }

        if (e->inst_data && !disable_instancing) {
            rq_submit_explicit_instanced(e);
            st.submits_out++;
            i++;
            continue;
        }
        if (e->inst_data && disable_instancing) {
            /* Explicit-instanced batch on D3D12: fan out as N single
             * submits using transform[0] (loses per-instance custom
             * data, but at least no crash).  Caller should avoid the
             * explicit_instanced API on D3D12 when this matters. */
            rq_submit_single(e);
            st.submits_out++;
            i++;
            continue;
        }

        uint32_t j = i + 1;
        while (j < rq->count && rq_can_batch(e, &rq->entries[j])) j++;

        uint32_t run = j - i;
        if (!disable_instancing && run >= 2 &&
            rq_submit_auto_batched(rq->entries, i, j)) {
            st.submits_out++;
            st.batches_merged++;
            st.instances_total += run;
        } else {
            for (uint32_t k = i; k < j; k++) {
                rq_submit_single(&rq->entries[k]);
                st.submits_out++;
            }
        }
        i = j;
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
        if (e->inst_data) {
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
