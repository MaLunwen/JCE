/*
 * jce_eqs.c — Environment Query System core (generators + tests + run).
 *
 * Pure CPU math; ZERO physics / navmesh / BT calls (those are injected as
 * CALLBACK tests by the caller).  See jce_eqs.h for the full contract.
 */

#include <jce/middleware/ai/jce_eqs.h>

#include "os/core/jce_memory.h"   /* JCE_CALLOC / JCE_FREE (mimalloc-tracked) */

#include <math.h>
#include <stdlib.h>

/* ----- handle ----------------------------------------------------- */

struct JceEqs {
    uint32_t           max_candidates;
    JceEqsScoredPoint *scratch; /* generation + scoring workspace */
};

/* ----- helpers ---------------------------------------------------- */

static float eqs_clamp01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

/* Number of candidates a generator emits (0 == malformed). */
uint32_t JCE_CALL jce_eqs_candidate_count(const JceEqsGeneratorDesc *gen)
{
    if (!gen)
        return 0u;

    switch (gen->kind) {
    case JCE_EQS_GEN_GRID:
        if (gen->grid_width == 0u || gen->grid_height == 0u)
            return 0u;
        return gen->grid_width * gen->grid_height;
    case JCE_EQS_GEN_RING:
        return gen->ring_point_count; /* 0 -> malformed */
    default:
        return 0u;
    }
}

/* Write candidate `index` (0-based) of the generator into *out.
 * Caller guarantees index < jce_eqs_candidate_count(gen). */
static jce_vec3 eqs_generate_point(const JceEqsGeneratorDesc *gen,
                                   uint32_t index)
{
    jce_vec3 p = gen->center;

    if (gen->kind == JCE_EQS_GEN_GRID) {
        uint32_t col = index % gen->grid_width;
        uint32_t row = index / gen->grid_width;
        /* Center the grid: offsets symmetric about center in XZ. */
        float fx = ((float)col - (float)(gen->grid_width - 1u) * 0.5f) *
                   gen->grid_spacing;
        float fz = ((float)row - (float)(gen->grid_height - 1u) * 0.5f) *
                   gen->grid_spacing;
        p.x = gen->center.x + fx;
        p.z = gen->center.z + fz;
    } else { /* JCE_EQS_GEN_RING */
        float theta = (2.0f * JCE_PI * (float)index) /
                      (float)gen->ring_point_count;
        p.x = gen->center.x + cosf(theta) * gen->ring_radius;
        p.z = gen->center.z + sinf(theta) * gen->ring_radius;
    }
    return p;
}

/* Raw value for a non-filter / filter test.  For CALLBACK filter tests the
 * raw is unused (the pass flag governs); we still return a stable value. */
static float eqs_test_raw(const JceEqsTestDesc *t,
                          jce_vec3 candidate,
                          const JceEqsGeneratorDesc *gen)
{
    switch (t->kind) {
    case JCE_EQS_TEST_DISTANCE:
        return jce_v3_len(jce_v3_sub(candidate, t->param_vec3));
    case JCE_EQS_TEST_DOT: {
        jce_vec3 origin = t->dot_origin;
        /* When dot_origin is all-zero AND differs from center handling is the
         * caller's concern; default origin is gen->center if dot_origin is
         * the zero vector. */
        if (origin.x == 0.0f && origin.y == 0.0f && origin.z == 0.0f)
            origin = gen->center;
        jce_vec3 dir = jce_v3_normalize(jce_v3_sub(candidate, origin));
        jce_vec3 ref = jce_v3_normalize(t->param_vec3);
        return jce_v3_dot(dir, ref);
    }
    case JCE_EQS_TEST_CALLBACK:
        if (t->value_fn)
            return t->value_fn(candidate, t->user);
        return 0.0f;
    default:
        return 0.0f;
    }
}

/* Map raw -> [0,1] through the test's curve.  Validation has already ensured
 * the curve params are well-formed (no divide-by-zero). */
static float eqs_apply_curve(const JceEqsTestDesc *t, float raw)
{
    switch (t->curve) {
    case JCE_EQS_CURVE_LINEAR:
        return eqs_clamp01(1.0f - raw / t->param_f1);
    case JCE_EQS_CURVE_INVERSE: {
        float r = (raw < 0.0f) ? 0.0f : raw;
        return 1.0f / (1.0f + r);
    }
    case JCE_EQS_CURVE_CLAMPED:
        return eqs_clamp01((raw - t->param_f0) /
                           (t->param_f1 - t->param_f0));
    case JCE_EQS_CURVE_CUSTOM:
        return t->score_fn ? eqs_clamp01(t->score_fn(raw)) : 0.0f;
    default:
        return 0.0f;
    }
}

static bool eqs_test_is_filter(const JceEqsTestDesc *t)
{
    return t->has_filter_min || t->has_filter_max;
}

/* ----- public API ------------------------------------------------- */

JceEqs *JCE_CALL jce_eqs_create(uint32_t max_candidates)
{
    JceEqs *eqs;

    if (max_candidates == 0u)
        return NULL;

    eqs = (JceEqs *)JCE_CALLOC(1u, sizeof(*eqs));
    if (!eqs)
        return NULL;

    eqs->scratch = (JceEqsScoredPoint *)JCE_CALLOC((size_t)max_candidates,
                                                   sizeof(JceEqsScoredPoint));
    if (!eqs->scratch) {
        JCE_FREE(eqs);
        return NULL;
    }
    eqs->max_candidates = max_candidates;
    return eqs;
}

void JCE_CALL jce_eqs_destroy(JceEqs *eqs)
{
    if (!eqs)
        return;
    JCE_FREE(eqs->scratch);
    JCE_FREE(eqs);
}

bool JCE_CALL jce_eqs_validate_query(const JceEqsQueryDesc *query)
{
    uint32_t i;

    if (!query)
        return false;
    if (jce_eqs_candidate_count(&query->generator) == 0u)
        return false;
    if (query->test_count > 0u && !query->tests)
        return false;

    for (i = 0u; i < query->test_count; ++i) {
        const JceEqsTestDesc *t = &query->tests[i];
        switch (t->curve) {
        case JCE_EQS_CURVE_LINEAR:
            if (!(t->param_f1 > 0.0f))
                return false;
            break;
        case JCE_EQS_CURVE_CLAMPED:
            if (!(t->param_f1 > t->param_f0))
                return false;
            break;
        case JCE_EQS_CURVE_CUSTOM:
            if (!t->score_fn)
                return false;
            break;
        case JCE_EQS_CURVE_INVERSE:
            break;
        default:
            return false;
        }
        if (t->kind == JCE_EQS_TEST_CALLBACK && !t->test_fn && !t->value_fn)
            return false;
    }
    return true;
}

uint32_t JCE_CALL jce_eqs_run(JceEqs *eqs, const JceEqsQueryDesc *query)
{
    uint32_t count;
    uint32_t survivors = 0u;
    uint32_t i;

    if (!eqs || !query)
        return 0u;
    if (!jce_eqs_validate_query(query))
        return 0u;

    count = jce_eqs_candidate_count(&query->generator);
    if (count > eqs->max_candidates)
        return 0u;

    /* Generate + score into scratch, dropping filtered candidates. */
    for (i = 0u; i < count; ++i) {
        jce_vec3 cand = eqs_generate_point(&query->generator, i);
        float    score = 0.0f;
        bool     keep = true;
        uint32_t ti;

        for (ti = 0u; ti < query->test_count; ++ti) {
            const JceEqsTestDesc *t = &query->tests[ti];
            float raw;
            float contrib;

            if (eqs_test_is_filter(t)) {
                if (t->kind == JCE_EQS_TEST_CALLBACK) {
                    /* Boolean filter form: callback decides pass directly. */
                    bool pass = t->test_fn ? t->test_fn(cand, t->user) : false;
                    if (!pass) {
                        keep = false;
                        break;
                    }
                    raw = 1.0f; /* passing candidates score full on the curve */
                } else {
                    raw = eqs_test_raw(t, cand, &query->generator);
                    if (t->has_filter_min && raw < t->filter_min) {
                        keep = false;
                        break;
                    }
                    if (t->has_filter_max && raw > t->filter_max) {
                        keep = false;
                        break;
                    }
                }
            } else {
                raw = eqs_test_raw(t, cand, &query->generator);
            }

            contrib = eqs_apply_curve(t, raw) * t->weight;
            score += contrib;
        }

        if (keep) {
            eqs->scratch[survivors].position = cand;
            eqs->scratch[survivors].score = score;
            ++survivors;
        }
    }

    /* Stable insertion sort, descending by score.  Stability keeps generator
     * order among equal scores -> fully deterministic output. */
    for (i = 1u; i < survivors; ++i) {
        JceEqsScoredPoint key = eqs->scratch[i];
        uint32_t j = i;
        while (j > 0u && eqs->scratch[j - 1u].score < key.score) {
            eqs->scratch[j] = eqs->scratch[j - 1u];
            --j;
        }
        eqs->scratch[j] = key;
    }

    /* Copy survivors out (clamped to caller capacity). */
    {
        uint32_t out_n = survivors;
        if (query->out_candidates && query->out_capacity < out_n)
            out_n = query->out_capacity;
        if (!query->out_candidates)
            out_n = 0u;
        for (i = 0u; i < out_n; ++i)
            query->out_candidates[i] = eqs->scratch[i];
        /* Report the number actually written. */
        return out_n;
    }
}

bool JCE_CALL jce_eqs_pick_best(JceEqs *eqs,
                                const JceEqsQueryDesc *query,
                                jce_vec3 *out_best,
                                float *out_score)
{
    uint32_t count;
    uint32_t survivors = 0u;
    uint32_t i;
    uint32_t best = 0u;
    bool     have_best = false;

    if (!eqs || !query)
        return false;
    if (!jce_eqs_validate_query(query))
        return false;

    count = jce_eqs_candidate_count(&query->generator);
    if (count > eqs->max_candidates)
        return false;

    for (i = 0u; i < count; ++i) {
        jce_vec3 cand = eqs_generate_point(&query->generator, i);
        float    score = 0.0f;
        bool     keep = true;
        uint32_t ti;

        for (ti = 0u; ti < query->test_count; ++ti) {
            const JceEqsTestDesc *t = &query->tests[ti];
            float raw;

            if (eqs_test_is_filter(t)) {
                if (t->kind == JCE_EQS_TEST_CALLBACK) {
                    bool pass = t->test_fn ? t->test_fn(cand, t->user) : false;
                    if (!pass) {
                        keep = false;
                        break;
                    }
                    raw = 1.0f;
                } else {
                    raw = eqs_test_raw(t, cand, &query->generator);
                    if (t->has_filter_min && raw < t->filter_min) {
                        keep = false;
                        break;
                    }
                    if (t->has_filter_max && raw > t->filter_max) {
                        keep = false;
                        break;
                    }
                }
            } else {
                raw = eqs_test_raw(t, cand, &query->generator);
            }
            score += eqs_apply_curve(t, raw) * t->weight;
        }

        if (keep) {
            eqs->scratch[survivors].position = cand;
            eqs->scratch[survivors].score = score;
            /* Track the highest score; first-seen wins ties (generator order). */
            if (!have_best || score > eqs->scratch[best].score) {
                best = survivors;
                have_best = true;
            }
            ++survivors;
        }
    }

    if (!have_best)
        return false;

    if (out_best)
        *out_best = eqs->scratch[best].position;
    if (out_score)
        *out_score = eqs->scratch[best].score;
    return true;
}
