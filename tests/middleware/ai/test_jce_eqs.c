/*
 * test_jce_eqs.c — Unit tests for jce_eqs.h (L3 AI, Environment Query System).
 *
 * The EQS core is pure math + callback-injected world tests.  These tests are
 * fully headless and deterministic (no nav/physics/BT): generators are exact
 * geometry, scoring curves map known raws to known [0,1], filters drop the
 * expected candidates, and the survivors come out sorted descending by score.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_eqs.h>
#include <jce/os/core/jce_math.h>

#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

static void assert_v3_near(jce_vec3 a, jce_vec3 b, float eps)
{
    TEST_ASSERT_FLOAT_WITHIN(eps, b.x, a.x);
    TEST_ASSERT_FLOAT_WITHIN(eps, b.y, a.y);
    TEST_ASSERT_FLOAT_WITHIN(eps, b.z, a.z);
}

/* ------------------------------------------------------------------ */
/* Generators                                                          */
/* ------------------------------------------------------------------ */

static void test_grid_count_and_layout(void)
{
    JceEqsGeneratorDesc g = { 0 };
    g.kind = JCE_EQS_GEN_GRID;
    g.center = jce_v3(0.0f, 5.0f, 0.0f); /* Y carried from center */
    g.grid_width = 5u;
    g.grid_height = 5u;
    g.grid_spacing = 1.0f;

    TEST_ASSERT_EQUAL_UINT32(25u, jce_eqs_candidate_count(&g));

    /* Run with no tests -> all 25 survive (score 0), sorted (stable). */
    JceEqsScoredPoint out[25];
    JceEqsQueryDesc q = { 0 };
    q.generator = g;
    q.tests = NULL;
    q.test_count = 0u;
    q.out_candidates = out;
    q.out_capacity = 25u;

    JceEqs *eqs = jce_eqs_create(64u);
    TEST_ASSERT_NOT_NULL(eqs);
    uint32_t n = jce_eqs_run(eqs, &q);
    TEST_ASSERT_EQUAL_UINT32(25u, n);

    /* First generated point is the (-2,-2) corner; Y preserved at 5. */
    assert_v3_near(out[0].position, jce_v3(-2.0f, 5.0f, -2.0f), EPS);
    /* Last is the (+2,+2) corner. */
    assert_v3_near(out[24].position, jce_v3(2.0f, 5.0f, 2.0f), EPS);

    jce_eqs_destroy(eqs);
}

static void test_ring_generator(void)
{
    JceEqsGeneratorDesc g = { 0 };
    g.kind = JCE_EQS_GEN_RING;
    g.center = jce_v3(0.0f, 0.0f, 0.0f);
    g.ring_radius = 3.0f;
    g.ring_point_count = 8u;

    TEST_ASSERT_EQUAL_UINT32(8u, jce_eqs_candidate_count(&g));

    JceEqsScoredPoint out[8];
    JceEqsQueryDesc q = { 0 };
    q.generator = g;
    q.out_candidates = out;
    q.out_capacity = 8u;

    JceEqs *eqs = jce_eqs_create(8u);
    uint32_t n = jce_eqs_run(eqs, &q);
    TEST_ASSERT_EQUAL_UINT32(8u, n);

    /* Every ring point is exactly ring_radius from the center. */
    for (uint32_t i = 0u; i < n; ++i) {
        float r = jce_v3_len(jce_v3_sub(out[i].position, g.center));
        TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, r);
        TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[i].position.y);
    }
    jce_eqs_destroy(eqs);
}

/* ------------------------------------------------------------------ */
/* Curves: known raw -> known [0,1]                                     */
/* ------------------------------------------------------------------ */

/* Probe a single curve by running a 1-point grid with one DISTANCE test
 * whose target sits `raw` away from the lone candidate at the origin. */
static float probe_curve_score(JceEqsCurveKind curve, float raw,
                               float f0, float f1, float weight)
{
    JceEqsGeneratorDesc g = { 0 };
    g.kind = JCE_EQS_GEN_GRID;
    g.center = jce_v3(0.0f, 0.0f, 0.0f);
    g.grid_width = 1u;
    g.grid_height = 1u;
    g.grid_spacing = 1.0f;

    JceEqsTestDesc t = { 0 };
    t.kind = JCE_EQS_TEST_DISTANCE;
    t.curve = curve;
    t.weight = weight;
    t.param_vec3 = jce_v3(raw, 0.0f, 0.0f); /* distance to origin == raw */
    t.param_f0 = f0;
    t.param_f1 = f1;

    JceEqsScoredPoint out[1];
    JceEqsQueryDesc q = { 0 };
    q.generator = g;
    q.tests = &t;
    q.test_count = 1u;
    q.out_candidates = out;
    q.out_capacity = 1u;

    JceEqs *eqs = jce_eqs_create(1u);
    uint32_t n = jce_eqs_run(eqs, &q);
    float s = (n == 1u) ? out[0].score : -1.0f;
    jce_eqs_destroy(eqs);
    return s;
}

static void test_curve_linear(void)
{
    /* LINEAR: clamp(1 - raw/f1). raw=2.5, f1=10 -> 0.75; weight 1. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f,
        probe_curve_score(JCE_EQS_CURVE_LINEAR, 2.5f, 0.0f, 10.0f, 1.0f));
    /* raw beyond f1 clamps to 0. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,
        probe_curve_score(JCE_EQS_CURVE_LINEAR, 20.0f, 0.0f, 10.0f, 1.0f));
    /* raw 0 -> 1. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f,
        probe_curve_score(JCE_EQS_CURVE_LINEAR, 0.0f, 0.0f, 10.0f, 1.0f));
}

static void test_curve_inverse(void)
{
    /* INVERSE: 1/(1+raw). raw=3 -> 0.25. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f,
        probe_curve_score(JCE_EQS_CURVE_INVERSE, 3.0f, 0.0f, 0.0f, 1.0f));
    /* raw=0 -> 1. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f,
        probe_curve_score(JCE_EQS_CURVE_INVERSE, 0.0f, 0.0f, 0.0f, 1.0f));
}

static void test_curve_clamped(void)
{
    /* CLAMPED: remap raw in [f0,f1]->[0,1]. raw=4, f0=2,f1=6 -> 0.5. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f,
        probe_curve_score(JCE_EQS_CURVE_CLAMPED, 4.0f, 2.0f, 6.0f, 1.0f));
    /* below f0 -> 0. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,
        probe_curve_score(JCE_EQS_CURVE_CLAMPED, 1.0f, 2.0f, 6.0f, 1.0f));
    /* above f1 -> 1. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f,
        probe_curve_score(JCE_EQS_CURVE_CLAMPED, 9.0f, 2.0f, 6.0f, 1.0f));
    /* weight multiplies: 0.5 * 4 = 2.0. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f,
        probe_curve_score(JCE_EQS_CURVE_CLAMPED, 4.0f, 2.0f, 6.0f, 4.0f));
}

/* ------------------------------------------------------------------ */
/* Pick best: farthest-from-origin corner within radius R              */
/* ------------------------------------------------------------------ */

static void test_pick_farthest_within_radius(void)
{
    /* 5x5 grid spacing 1 about origin -> x,z in {-2,-1,0,1,2}.
     * Test A (scoring): prefer FAR from target at origin. Use CLAMPED on
     *   distance with f0=0, f1=4 so score = clamp(dist/4). The farthest grid
     *   distance overall is a corner at (+-2,+-2): dist = sqrt(8) = 2.828427.
     * Test B (filter): DISTANCE from center must be <= R = 3.0 (corners at
     *   2.828 < 3 pass; nothing is excluded here, but the filter is live).
     * Expected best = a corner, score = clamp(2.828427/4) = 0.7071068. */
    JceEqsGeneratorDesc g = { 0 };
    g.kind = JCE_EQS_GEN_GRID;
    g.center = jce_v3(0.0f, 0.0f, 0.0f);
    g.grid_width = 5u;
    g.grid_height = 5u;
    g.grid_spacing = 1.0f;

    JceEqsTestDesc tests[2];
    /* A: far-from-origin (scoring, CLAMPED). */
    tests[0] = (JceEqsTestDesc){ 0 };
    tests[0].kind = JCE_EQS_TEST_DISTANCE;
    tests[0].curve = JCE_EQS_CURVE_CLAMPED;
    tests[0].weight = 1.0f;
    tests[0].param_vec3 = jce_v3(0.0f, 0.0f, 0.0f);
    tests[0].param_f0 = 0.0f;
    tests[0].param_f1 = 4.0f;
    /* B: within-radius-3 filter (distance from center). */
    tests[1] = (JceEqsTestDesc){ 0 };
    tests[1].kind = JCE_EQS_TEST_DISTANCE;
    tests[1].curve = JCE_EQS_CURVE_INVERSE; /* curve irrelevant; weight 0 */
    tests[1].weight = 0.0f;
    tests[1].param_vec3 = jce_v3(0.0f, 0.0f, 0.0f);
    tests[1].has_filter_max = true;
    tests[1].filter_max = 3.0f;

    JceEqsScoredPoint out[25];
    JceEqsQueryDesc q = { 0 };
    q.generator = g;
    q.tests = tests;
    q.test_count = 2u;
    q.out_candidates = out;
    q.out_capacity = 25u;

    JceEqs *eqs = jce_eqs_create(64u);
    TEST_ASSERT_TRUE(jce_eqs_validate_query(&q));

    jce_vec3 best;
    float    best_score;
    TEST_ASSERT_TRUE(jce_eqs_pick_best(eqs, &q, &best, &best_score));

    /* Best must be a corner |x|==2 && |z|==2. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, fabsf(best.x));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, fabsf(best.z));
    /* Hand-computed score: sqrt(8)/4 = 0.70710678. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.70710678f, best_score);

    /* The first generated corner (-2,-2) wins ties -> deterministic. */
    assert_v3_near(best, jce_v3(-2.0f, 0.0f, -2.0f), EPS);

    /* run() agrees and is sorted descending. */
    uint32_t n = jce_eqs_run(eqs, &q);
    TEST_ASSERT_EQUAL_UINT32(25u, n); /* nothing filtered (all within 3) */
    assert_v3_near(out[0].position, jce_v3(-2.0f, 0.0f, -2.0f), EPS);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.70710678f, out[0].score);
    for (uint32_t i = 1u; i < n; ++i)
        TEST_ASSERT_TRUE(out[i - 1u].score >= out[i].score - EPS);

    jce_eqs_destroy(eqs);
}

static void test_filter_tightens_radius(void)
{
    /* Same grid, but filter radius R = 2.0: corners (2.828) and edge-mids of
     * the long diagonals fail; only points with dist<=2 survive. Count them:
     * points (x,z) with x^2+z^2 <= 4, x,z in {-2,-1,0,1,2}:
     *   include (0,0); (+-1,0)x2; (0,+-1)x2; (+-2,0)x2; (0,+-2)x2;
     *           (+-1,+-1)x4 (dist sqrt2<=2). Total = 1+4+4+4 = 13.
     * (+-2,+-1) dist sqrt5 ~ 2.236 > 2 excluded; corners excluded. */
    JceEqsGeneratorDesc g = { 0 };
    g.kind = JCE_EQS_GEN_GRID;
    g.center = jce_v3(0.0f, 0.0f, 0.0f);
    g.grid_width = 5u;
    g.grid_height = 5u;
    g.grid_spacing = 1.0f;

    JceEqsTestDesc t = { 0 };
    t.kind = JCE_EQS_TEST_DISTANCE;
    t.curve = JCE_EQS_CURVE_INVERSE;
    t.weight = 1.0f;
    t.param_vec3 = jce_v3(0.0f, 0.0f, 0.0f);
    t.has_filter_max = true;
    t.filter_max = 2.0f;

    JceEqsScoredPoint out[25];
    JceEqsQueryDesc q = { 0 };
    q.generator = g;
    q.tests = &t;
    q.test_count = 1u;
    q.out_candidates = out;
    q.out_capacity = 25u;

    JceEqs *eqs = jce_eqs_create(64u);
    uint32_t n = jce_eqs_run(eqs, &q);
    TEST_ASSERT_EQUAL_UINT32(13u, n);
    /* INVERSE favors small distance -> center (0,0) tops the list, score 1. */
    assert_v3_near(out[0].position, jce_v3(0.0f, 0.0f, 0.0f), EPS);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0].score);
    jce_eqs_destroy(eqs);
}

/* ------------------------------------------------------------------ */
/* CALLBACK filter excludes the right candidates (reject x < 0)         */
/* ------------------------------------------------------------------ */

static bool keep_nonneg_x(jce_vec3 c, void *user)
{
    (void)user;
    return c.x >= 0.0f;
}

static void test_callback_filter_rejects_negative_x(void)
{
    /* 5x1 grid spacing 1 about origin -> x in {-2,-1,0,1,2}, z=0.
     * Callback filter keeps x>=0 -> {0,1,2} survive (3 of 5). */
    JceEqsGeneratorDesc g = { 0 };
    g.kind = JCE_EQS_GEN_GRID;
    g.center = jce_v3(0.0f, 0.0f, 0.0f);
    g.grid_width = 5u;
    g.grid_height = 1u;
    g.grid_spacing = 1.0f;

    JceEqsTestDesc t = { 0 };
    t.kind = JCE_EQS_TEST_CALLBACK;
    t.curve = JCE_EQS_CURVE_INVERSE; /* passing candidates get raw=1 -> 0.5 */
    t.weight = 1.0f;
    t.test_fn = keep_nonneg_x;
    t.has_filter_min = true; /* mark as a filter test */
    t.filter_min = 0.0f;     /* unused by callback path, but flags filter */

    JceEqsScoredPoint out[5];
    JceEqsQueryDesc q = { 0 };
    q.generator = g;
    q.tests = &t;
    q.test_count = 1u;
    q.out_candidates = out;
    q.out_capacity = 5u;

    JceEqs *eqs = jce_eqs_create(8u);
    uint32_t n = jce_eqs_run(eqs, &q);
    TEST_ASSERT_EQUAL_UINT32(3u, n);
    for (uint32_t i = 0u; i < n; ++i)
        TEST_ASSERT_TRUE(out[i].position.x >= -EPS);
    jce_eqs_destroy(eqs);
}

/* ------------------------------------------------------------------ */
/* validate_query rejects ill-formed curves                            */
/* ------------------------------------------------------------------ */

static void test_validate_rejects_linear_zero_f1(void)
{
    JceEqsGeneratorDesc g = { 0 };
    g.kind = JCE_EQS_GEN_GRID;
    g.center = jce_v3(0.0f, 0.0f, 0.0f);
    g.grid_width = 2u;
    g.grid_height = 2u;
    g.grid_spacing = 1.0f;

    JceEqsTestDesc t = { 0 };
    t.kind = JCE_EQS_TEST_DISTANCE;
    t.curve = JCE_EQS_CURVE_LINEAR;
    t.weight = 1.0f;
    t.param_f1 = 0.0f; /* divide-by-zero -> must be rejected */

    JceEqsQueryDesc q = { 0 };
    q.generator = g;
    q.tests = &t;
    q.test_count = 1u;

    TEST_ASSERT_FALSE(jce_eqs_validate_query(&q));

    /* run() must also fail loudly (return 0). */
    JceEqsScoredPoint out[4];
    q.out_candidates = out;
    q.out_capacity = 4u;
    JceEqs *eqs = jce_eqs_create(4u);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_eqs_run(eqs, &q));
    jce_eqs_destroy(eqs);

    /* CLAMPED with f1<=f0 also rejected. */
    t.curve = JCE_EQS_CURVE_CLAMPED;
    t.param_f0 = 5.0f;
    t.param_f1 = 5.0f;
    TEST_ASSERT_FALSE(jce_eqs_validate_query(&q));

    /* A well-formed LINEAR passes. */
    t.curve = JCE_EQS_CURVE_LINEAR;
    t.param_f1 = 10.0f;
    TEST_ASSERT_TRUE(jce_eqs_validate_query(&q));
}

/* ------------------------------------------------------------------ */
/* NULL-safety                                                          */
/* ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    TEST_ASSERT_EQUAL_UINT32(0u, jce_eqs_candidate_count(NULL));
    TEST_ASSERT_FALSE(jce_eqs_validate_query(NULL));
    TEST_ASSERT_NULL(jce_eqs_create(0u));

    JceEqs *eqs = jce_eqs_create(4u);
    TEST_ASSERT_NOT_NULL(eqs);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_eqs_run(eqs, NULL));
    TEST_ASSERT_FALSE(jce_eqs_pick_best(eqs, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_eqs_run(NULL, NULL));
    jce_eqs_destroy(eqs);
    jce_eqs_destroy(NULL); /* no crash */
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_grid_count_and_layout);
    RUN_TEST(test_ring_generator);
    RUN_TEST(test_curve_linear);
    RUN_TEST(test_curve_inverse);
    RUN_TEST(test_curve_clamped);
    RUN_TEST(test_pick_farthest_within_radius);
    RUN_TEST(test_filter_tightens_radius);
    RUN_TEST(test_callback_filter_rejects_negative_x);
    RUN_TEST(test_validate_rejects_linear_zero_f1);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
