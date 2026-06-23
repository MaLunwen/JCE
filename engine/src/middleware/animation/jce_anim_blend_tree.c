/*
 * jce_anim_blend_tree.c  Animation blend trees implementation.
 */

#include <jce/middleware/animation/jce_anim_blend_tree.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <jce/os/core/jce_str.h>

typedef struct {
    char               name[64];      /* clip lookup key                  */
    float              threshold;     /* 1D: parameter value for full wt  */
    float              x;             /* 2D: sample position x            */
    float              y;             /* 2D: sample position y            */
    const JceAnimClip *clip;          /* bound by set_clip(); may be NULL */
} BtEntry;

struct JceAnimBlendTree {
    BtEntry             *entries;
    uint32_t             count;
    uint32_t             cap;
    bool                 sorted;       /* 1D lazy-sort flag                */
    JceAnimBlendTreeMode mode;         /* 1D / 2D-cartesian / 2D-direct.   */
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
    bt->mode   = JCE_BLEND_TREE_1D;
    return bt;
}

JceAnimBlendTree *jce_anim_blend_tree_create_2d(uint32_t capacity,
                                                bool directional)
{
    if (capacity == 0) capacity = 4;
    if (capacity > JCE_BLEND_TREE_2D_MAX_SAMPLES)
        capacity = JCE_BLEND_TREE_2D_MAX_SAMPLES;
    JceAnimBlendTree *bt = (JceAnimBlendTree *)JCE_CALLOC(1, sizeof(*bt));
    if (!bt) return NULL;
    bt->entries = (BtEntry *)JCE_CALLOC(capacity, sizeof(BtEntry));
    if (!bt->entries) { JCE_FREE(bt); return NULL; }
    bt->cap    = capacity;
    bt->sorted = true;      /* unused for 2D, but keep deterministic       */
    bt->mode   = directional ? JCE_BLEND_TREE_2D_DIRECTIONAL
                             : JCE_BLEND_TREE_2D_CARTESIAN;
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

int jce_anim_blend_tree_add_2d(JceAnimBlendTree *bt,
                                const char *clip_name,
                                float x, float y)
{
    if (!bt || !clip_name) return -1;
    if (bt->mode != JCE_BLEND_TREE_2D_CARTESIAN &&
        bt->mode != JCE_BLEND_TREE_2D_DIRECTIONAL)
        return -1;
    if (bt->count >= bt->cap) return -1;

    BtEntry *e = &bt->entries[bt->count];
    jce_strlcpy(e->name, clip_name, sizeof(e->name));
    e->threshold = 0.0f;
    e->x         = x;
    e->y         = y;
    e->clip      = NULL;

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

JceAnimBlendTreeMode jce_anim_blend_tree_mode(const JceAnimBlendTree *bt)
{
    return bt ? bt->mode : JCE_BLEND_TREE_1D;
}

void jce_anim_blend_tree_position(const JceAnimBlendTree *bt, uint32_t i,
                                  float *out_x, float *out_y)
{
    float x = 0.0f, y = 0.0f;
    if (bt && i < bt->count) {
        x = bt->entries[i].x;
        y = bt->entries[i].y;
    }
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
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

/* ── 2D evaluation (gradient-band interpolation) ─────────────────────
 *
 * Unity-style Gradient Band Interpolation (Rune Skovbo Johansen, 2009).
 * For each sample i, its weight starts at 1 and is reduced by every
 * other sample j according to how far the query has moved from i toward
 * j.  The minimum of these per-j factors is sample i's raw weight; the
 * raw weights are then normalised to sum to 1.
 *
 *   Pip = q - pi          (query relative to sample i)
 *   Pij = pj - pi         (sample j relative to sample i)
 *   h   = 1 - dot(Pip,Pij)/dot(Pij,Pij)        (clamped to [0,1])
 *   w_i = min over j != i of h
 *
 * Freeform Cartesian uses raw (x,y).  Freeform Directional maps every
 * point and every pairwise vector into a polar (angle,magnitude) space
 * so opposite-facing motions (e.g. walk-forward vs walk-back) never
 * bleed into each other.
 */

static float clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Directional vector-between, per Unity / Johansen: the influence of a
 * move from sample `a` toward sample `b`, expressed in (angle,magnitude)
 * polar coordinates.  Returns the 2D "Pij"-style vector for the pair and,
 * via out_pip, the corresponding query vector "Pip" for sample `a`.      */
static void dir_vectors(float ax, float ay, float bx, float by,
                        float qx, float qy,
                        float out_pij[2], float out_pip[2])
{
    const float kPi = 3.14159265358979323846f;
    float la = sqrtf(ax * ax + ay * ay);
    float lb = sqrtf(bx * bx + by * by);
    float lq = sqrtf(qx * qx + qy * qy);

    float avg_len = 0.5f * (la + lb);

    /* angle a→b (signed, atan2 of the 2D cross/dot) and a→q */
    float angle_ab, angle_aq;
    if (la < 1e-6f || lb < 1e-6f) {
        angle_ab = 0.0f;
    } else {
        float dotv = (ax * bx + ay * by) / (la * lb);
        float crsv = (ax * by - ay * bx) / (la * lb);
        angle_ab = atan2f(crsv, dotv);
    }
    if (la < 1e-6f || lq < 1e-6f) {
        angle_aq = 0.0f;
    } else {
        float dotv = (ax * qx + ay * qy) / (la * lq);
        float crsv = (ax * qy - ay * qx) / (la * lq);
        angle_aq = atan2f(crsv, dotv);
    }

    /* Pij: (radial length delta, angular delta) scaled like Unity. */
    out_pij[0] = (lb - la);
    out_pij[1] = angle_ab * avg_len;
    /* Pip: query relative to sample a in the same polar frame. */
    out_pip[0] = (lq - la);
    out_pip[1] = angle_aq * avg_len;

    /* avoid unused warning on kPi for compilers; keep the constant
       available for future weighting tweaks. */
    (void)kPi;
}

void jce_anim_blend_tree_eval_2d(const JceAnimBlendTree *bt,
                                 float x, float y,
                                 JceAnimBlendTreeEval2D *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    if (!bt) return;
    if (bt->mode != JCE_BLEND_TREE_2D_CARTESIAN &&
        bt->mode != JCE_BLEND_TREE_2D_DIRECTIONAL)
        return;
    if (bt->count == 0) return;

    uint32_t n = bt->count;
    if (n > JCE_BLEND_TREE_2D_MAX_SAMPLES) n = JCE_BLEND_TREE_2D_MAX_SAMPLES;

    bool directional = (bt->mode == JCE_BLEND_TREE_2D_DIRECTIONAL);

    float raw[JCE_BLEND_TREE_2D_MAX_SAMPLES];
    float total = 0.0f;

    for (uint32_t i = 0; i < n; ++i) {
        float w = 1.0f;
        float pix = bt->entries[i].x;
        float piy = bt->entries[i].y;

        for (uint32_t j = 0; j < n; ++j) {
            if (j == i) continue;
            float pjx = bt->entries[j].x;
            float pjy = bt->entries[j].y;

            float pij[2], pip[2];
            if (directional) {
                dir_vectors(pix, piy, pjx, pjy, x, y, pij, pip);
            } else {
                pij[0] = pjx - pix;  pij[1] = pjy - piy;   /* pj - pi  */
                pip[0] = x   - pix;  pip[1] = y   - piy;   /* q  - pi  */
            }

            float denom = pij[0] * pij[0] + pij[1] * pij[1];
            float h;
            if (denom < 1e-12f) {
                /* coincident samples: no separation, full influence. */
                h = 1.0f;
            } else {
                float dot = pip[0] * pij[0] + pip[1] * pij[1];
                h = clamp01f(1.0f - dot / denom);
            }
            if (h < w) w = h;
        }

        raw[i] = w;
        total += w;
    }

    out->count = n;
    if (total > 1e-9f) {
        for (uint32_t i = 0; i < n; ++i) {
            out->weights[i] = raw[i] / total;
            out->clips[i]   = bt->entries[i].clip;
        }
    } else {
        /* Degenerate (e.g. single sample, or query coincident with all):
           fall back to the nearest sample carrying all the weight. */
        uint32_t best = 0;
        float    best_d2 = 1e30f;
        for (uint32_t i = 0; i < n; ++i) {
            float dx = x - bt->entries[i].x;
            float dy = y - bt->entries[i].y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; best = i; }
            out->clips[i] = bt->entries[i].clip;
        }
        for (uint32_t i = 0; i < n; ++i)
            out->weights[i] = (i == best) ? 1.0f : 0.0f;
    }
}
