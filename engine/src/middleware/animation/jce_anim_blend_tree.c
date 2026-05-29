/*
 * jce_anim_blend_tree.c  Animation blend trees implementation.
 */

#include <jce/middleware/animation/jce_anim_blend_tree.h>

#include "os/core/jce_memory.h"

#include <stdlib.h>
#include <string.h>
#include <jce/os/core/jce_str.h>

typedef struct {
    char               name[64];      /* clip lookup key                  */
    float              threshold;     /* parameter value for full weight  */
    const JceAnimClip *clip;          /* bound by set_clip(); may be NULL */
} BtEntry;

struct JceAnimBlendTree {
    BtEntry *entries;
    uint32_t count;
    uint32_t cap;
    bool     sorted;
};

/* ── Construction ────────────────────────────────────────────────── */

JceAnimBlendTree *jce_anim_blend_tree_create_1d(uint32_t capacity)
{
    if (capacity == 0) capacity = 4;
    JceAnimBlendTree *bt = (JceAnimBlendTree *)JCE_CALLOC(1, sizeof(*bt));
    if (!bt) return NULL;
    bt->entries = (BtEntry *)JCE_CALLOC(capacity, sizeof(BtEntry));
    if (!bt->entries) { JCE_FREE(bt); return NULL; }
    bt->cap    = capacity;
    bt->sorted = true;
    return bt;
}

void jce_anim_blend_tree_destroy(JceAnimBlendTree *bt)
{
    if (!bt) return;
    JCE_FREE(bt->entries);
    JCE_FREE(bt);
}

int jce_anim_blend_tree_add(JceAnimBlendTree *bt,
                             const char *clip_name,
                             float threshold)
{
    if (!bt || !clip_name) return -1;
    if (bt->count >= bt->cap) return -1;

    BtEntry *e = &bt->entries[bt->count];
    jce_strlcpy(e->name, clip_name, sizeof(e->name));
    e->threshold = threshold;
    e->clip      = NULL;

    if (bt->count > 0 && threshold < bt->entries[bt->count - 1].threshold)
        bt->sorted = false;

    return (int)bt->count++;
}

bool jce_anim_blend_tree_set_clip(JceAnimBlendTree *bt,
                                   const char *clip_name,
                                   const JceAnimClip *clip)
{
    if (!bt || !clip_name) return false;
    for (uint32_t i = 0; i < bt->count; ++i) {
        if (strcmp(bt->entries[i].name, clip_name) == 0) {
            bt->entries[i].clip = clip;
            return true;
        }
    }
    return false;
}

uint32_t jce_anim_blend_tree_count(const JceAnimBlendTree *bt)
{
    return bt ? bt->count : 0;
}

const char *jce_anim_blend_tree_name(const JceAnimBlendTree *bt, uint32_t i)
{
    if (!bt || i >= bt->count) return NULL;
    return bt->entries[i].name;
}

float jce_anim_blend_tree_threshold(const JceAnimBlendTree *bt, uint32_t i)
{
    if (!bt || i >= bt->count) return 0.0f;
    return bt->entries[i].threshold;
}

/* ── Evaluation ──────────────────────────────────────────────────── */

static int cmp_entry(const void *a, const void *b)
{
    float ta = ((const BtEntry *)a)->threshold;
    float tb = ((const BtEntry *)b)->threshold;
    if (ta < tb) return -1;
    if (ta > tb) return  1;
    return 0;
}

void jce_anim_blend_tree_evaluate(const JceAnimBlendTree *bt,
                                   float value,
                                   JceAnimBlendTreeEval *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->index_a = -1;
    out->index_b = -1;

    if (!bt || bt->count == 0) return;

    /* Lazy sort: the public API allows out-of-order add(); sort once. */
    if (!bt->sorted) {
        qsort(bt->entries, bt->count, sizeof(BtEntry), cmp_entry);
        ((JceAnimBlendTree *)bt)->sorted = true;
    }

    /* Below the lowest threshold → snap to entry 0. */
    if (value <= bt->entries[0].threshold) {
        out->clip_a   = bt->entries[0].clip;
        out->clip_b   = bt->entries[0].clip;
        out->weight_a = 1.0f;
        out->weight_b = 0.0f;
        out->index_a  = 0;
        out->index_b  = 0;
        return;
    }
    /* Above the highest → snap to last entry. */
    uint32_t last = bt->count - 1;
    if (value >= bt->entries[last].threshold) {
        out->clip_a   = bt->entries[last].clip;
        out->clip_b   = bt->entries[last].clip;
        out->weight_a = 1.0f;
        out->weight_b = 0.0f;
        out->index_a  = (int)last;
        out->index_b  = (int)last;
        return;
    }

    /* Find bracketing pair (linear: blend trees are typically tiny). */
    for (uint32_t i = 0; i + 1 < bt->count; ++i) {
        float t0 = bt->entries[i].threshold;
        float t1 = bt->entries[i + 1].threshold;
        if (value >= t0 && value <= t1) {
            float span = t1 - t0;
            float w    = (span > 1e-6f) ? (value - t0) / span : 0.0f;
            out->clip_a   = bt->entries[i].clip;
            out->clip_b   = bt->entries[i + 1].clip;
            out->weight_a = 1.0f - w;
            out->weight_b = w;
            out->index_a  = (int)i;
            out->index_b  = (int)(i + 1);
            return;
        }
    }
}
