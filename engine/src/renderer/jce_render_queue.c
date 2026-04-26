/*
 * jce_render_queue.c  Sorted draw-call submission queue.
 *
 * Collects JceDrawCmd entries, sorts them by the requested mode,
 * and flushes to bgfx in a single pass to minimize state changes.
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

/* ── Internal struct ──────────────────────────────────────────────── */

struct JceRenderQueue {
    JceDrawCmd *cmds;
    uint32_t    count;
    uint32_t    capacity;
};

/* ── Sort comparators ─────────────────────────────────────────────── */

static int cmp_front_to_back(const void *a, const void *b)
{
    const JceDrawCmd *ca = (const JceDrawCmd *)a;
    const JceDrawCmd *cb = (const JceDrawCmd *)b;
    if (ca->depth < cb->depth) return -1;
    if (ca->depth > cb->depth) return  1;
    /* Secondary: group by material to reduce state changes. */
    if (ca->material_key < cb->material_key) return -1;
    if (ca->material_key > cb->material_key) return  1;
    return 0;
}

static int cmp_back_to_front(const void *a, const void *b)
{
    const JceDrawCmd *ca = (const JceDrawCmd *)a;
    const JceDrawCmd *cb = (const JceDrawCmd *)b;
    if (ca->depth > cb->depth) return -1;
    if (ca->depth < cb->depth) return  1;
    return 0;
}

static int cmp_by_material(const void *a, const void *b)
{
    const JceDrawCmd *ca = (const JceDrawCmd *)a;
    const JceDrawCmd *cb = (const JceDrawCmd *)b;
    if (ca->material_key < cb->material_key) return -1;
    if (ca->material_key > cb->material_key) return  1;
    /* Secondary: front-to-back within same material. */
    if (ca->depth < cb->depth) return -1;
    if (ca->depth > cb->depth) return  1;
    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

JceRenderQueue *jce_rq_create(uint32_t initial_capacity)
{
    if (initial_capacity == 0) initial_capacity = INITIAL_CAP_DEFAULT;

    JceRenderQueue *rq = (JceRenderQueue *)JCE_CALLOC(1, sizeof(*rq));
    if (!rq) return NULL;

    rq->cmds = (JceDrawCmd *)JCE_MALLOC(initial_capacity * sizeof(JceDrawCmd));
    if (!rq->cmds) {
        JCE_FREE(rq);
        return NULL;
    }

    rq->capacity = initial_capacity;
    rq->count    = 0;
    return rq;
}

void jce_rq_destroy(JceRenderQueue *rq)
{
    if (!rq) return;
    JCE_FREE(rq->cmds);
    JCE_FREE(rq);
}

/* ── Submission ───────────────────────────────────────────────────── */

void jce_rq_push(JceRenderQueue *rq, const JceDrawCmd *cmd)
{
    if (!rq || !cmd) return;

    /* Grow if needed (2x strategy). */
    if (rq->count >= rq->capacity) {
        uint32_t new_cap = rq->capacity * 2;
        JceDrawCmd *new_buf = (JceDrawCmd *)JCE_MALLOC(
            new_cap * sizeof(JceDrawCmd));
        if (!new_buf) {
            LOG_WARN(LOG_TAG, "failed to grow queue (cap=%u)", rq->capacity);
            return;
        }
        memcpy(new_buf, rq->cmds, rq->count * sizeof(JceDrawCmd));
        JCE_FREE(rq->cmds);
        rq->cmds     = new_buf;
        rq->capacity = new_cap;
    }

    rq->cmds[rq->count++] = *cmd;
}

void jce_rq_sort(JceRenderQueue *rq, JceSortMode mode)
{
    JCE_PROFILE_ZONE_N("RenderQueue::Sort");
    if (!rq || rq->count < 2) { JCE_PROFILE_ZONE_END; return; }

    switch (mode) {
    case JCE_SORT_FRONT_TO_BACK:
        qsort(rq->cmds, rq->count, sizeof(JceDrawCmd), cmp_front_to_back);
        break;
    case JCE_SORT_BACK_TO_FRONT:
        qsort(rq->cmds, rq->count, sizeof(JceDrawCmd), cmp_back_to_front);
        break;
    case JCE_SORT_BY_MATERIAL:
        qsort(rq->cmds, rq->count, sizeof(JceDrawCmd), cmp_by_material);
        break;
    case JCE_SORT_SEQUENTIAL:
        /* No sort — preserve insertion order. */
        break;
    }
    JCE_PROFILE_ZONE_END;
}

void jce_rq_flush(JceRenderQueue *rq, const JceRenderer *renderer)
{
    JCE_PROFILE_ZONE_N("RenderQueue::Flush");
    if (!rq || rq->count == 0) { JCE_PROFILE_ZONE_END; return; }
    (void)renderer;

    for (uint32_t i = 0; i < rq->count; i++) {
        const JceDrawCmd *c = &rq->cmds[i];

        bgfx_set_transform(c->transform.raw[0], 1);

        bgfx_vertex_buffer_handle_t vbh = { (uint16_t)c->mesh_vbh };
        bgfx_index_buffer_handle_t  ibh = { (uint16_t)c->mesh_ibh };

        bgfx_set_vertex_buffer(0, vbh, 0, UINT32_MAX);
        bgfx_set_index_buffer(ibh, 0, c->index_count);

        /* Convert float depth to uint32 sort key for bgfx.
         * IEEE 754 positive floats sort correctly as uint32. */
        uint32_t depth_key;
        memcpy(&depth_key, &c->depth, sizeof(depth_key));
        if (depth_key & 0x80000000u) {
            depth_key = ~depth_key;          /* negative: flip all bits */
        } else {
            depth_key |= 0x80000000u;        /* positive: flip sign bit */
        }

        bgfx_program_handle_t prog = { c->program };
        bgfx_submit(c->view_id, prog, depth_key, BGFX_DISCARD_ALL);
    }

    rq->count = 0;
    JCE_PROFILE_ZONE_END;
}

void jce_rq_clear(JceRenderQueue *rq)
{
    if (rq) rq->count = 0;
}

uint32_t jce_rq_count(const JceRenderQueue *rq)
{
    return rq ? rq->count : 0;
}
