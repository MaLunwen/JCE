/*
 * test_jce_anim_blend_tree_2d.c — Unit tests for the 2D blend tree (L3).
 *
 * A 2D blend tree positions each sample at an (x,y) point in parameter
 * space and, given a query point, produces a normalised per-sample weight
 * vector (summing to 1) via Unity-style gradient-band interpolation.
 *
 * These tests exercise the REAL eval (jce_anim_blend_tree_eval_2d), not a
 * mock.  Both weighting schemes (Freeform Cartesian + Freeform
 * Directional) are covered.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_anim_blend_tree.h>

#include <stddef.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* JceAnimClip is opaque; tests only need stable pointer identity. */
static char g_clip_storage[8];

#define CLIP(n) ((const JceAnimClip *)&g_clip_storage[(n)])

/* sum of all valid weights */
static float weight_sum(const JceAnimBlendTreeEval2D *e)
{
    float s = 0.0f;
    for (uint32_t i = 0; i < e->count; ++i) s += e->weights[i];
    return s;
}

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static void test_create_2d_cartesian_mode(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(4, false);
    TEST_ASSERT_NOT_NULL(bt);
    TEST_ASSERT_EQUAL_INT(JCE_BLEND_TREE_2D_CARTESIAN,
                          jce_anim_blend_tree_mode(bt));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_anim_blend_tree_count(bt));
    jce_anim_blend_tree_destroy(bt);
}

static void test_create_2d_directional_mode(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(4, true);
    TEST_ASSERT_NOT_NULL(bt);
    TEST_ASSERT_EQUAL_INT(JCE_BLEND_TREE_2D_DIRECTIONAL,
                          jce_anim_blend_tree_mode(bt));
    jce_anim_blend_tree_destroy(bt);
}

static void test_add_2d_returns_sequential_indices(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(4, false);
    TEST_ASSERT_EQUAL_INT(0, jce_anim_blend_tree_add_2d(bt, "fwd",   0.0f,  1.0f));
    TEST_ASSERT_EQUAL_INT(1, jce_anim_blend_tree_add_2d(bt, "left", -1.0f,  0.0f));
    TEST_ASSERT_EQUAL_INT(2, jce_anim_blend_tree_add_2d(bt, "right", 1.0f,  0.0f));
    TEST_ASSERT_EQUAL_UINT32(3u, jce_anim_blend_tree_count(bt));

    float px = 0.0f, py = 0.0f;
    jce_anim_blend_tree_position(bt, 0, &px, &py);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, px);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, py);
    jce_anim_blend_tree_destroy(bt);
}

static void test_add_2d_full_returns_minus_one(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(2, false);
    (void)jce_anim_blend_tree_add_2d(bt, "a", 0.0f, 0.0f);
    (void)jce_anim_blend_tree_add_2d(bt, "b", 1.0f, 0.0f);
    TEST_ASSERT_EQUAL_INT(-1, jce_anim_blend_tree_add_2d(bt, "c", 2.0f, 0.0f));
    jce_anim_blend_tree_destroy(bt);
}

static void test_add_2d_rejects_1d_tree(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    TEST_ASSERT_EQUAL_INT(-1, jce_anim_blend_tree_add_2d(bt, "a", 0.0f, 0.0f));
    jce_anim_blend_tree_destroy(bt);
}

/* ------------------------------------------------------------------ */
/* Cartesian evaluation                                                */
/* ------------------------------------------------------------------ */

/* Helper: build a 4-sample square (corners) Cartesian tree. */
static JceAnimBlendTree *make_square_cartesian(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(4, false);
    (void)jce_anim_blend_tree_add_2d(bt, "tl", -1.0f,  1.0f); /* 0 */
    (void)jce_anim_blend_tree_add_2d(bt, "tr",  1.0f,  1.0f); /* 1 */
    (void)jce_anim_blend_tree_add_2d(bt, "bl", -1.0f, -1.0f); /* 2 */
    (void)jce_anim_blend_tree_add_2d(bt, "br",  1.0f, -1.0f); /* 3 */
    (void)jce_anim_blend_tree_set_clip(bt, "tl", CLIP(0));
    (void)jce_anim_blend_tree_set_clip(bt, "tr", CLIP(1));
    (void)jce_anim_blend_tree_set_clip(bt, "bl", CLIP(2));
    (void)jce_anim_blend_tree_set_clip(bt, "br", CLIP(3));
    return bt;
}

static void test_eval_at_sample_point_is_one_there_zero_elsewhere(void)
{
    JceAnimBlendTree *bt = make_square_cartesian();
    JceAnimBlendTreeEval2D e = {0};

    /* Query exactly at sample 1 (top-right). */
    jce_anim_blend_tree_eval_2d(bt, 1.0f, 1.0f, &e);
    TEST_ASSERT_EQUAL_UINT32(4u, e.count);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, e.weights[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, e.weights[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, e.weights[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, e.weights[3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));
    /* clip mirror is populated */
    TEST_ASSERT_EQUAL_PTR(CLIP(1), e.clips[1]);
    jce_anim_blend_tree_destroy(bt);
}

static void test_eval_each_sample_point_dominates(void)
{
    JceAnimBlendTree *bt = make_square_cartesian();
    for (uint32_t s = 0; s < 4; ++s) {
        float px = 0.0f, py = 0.0f;
        jce_anim_blend_tree_position(bt, s, &px, &py);
        JceAnimBlendTreeEval2D e = {0};
        jce_anim_blend_tree_eval_2d(bt, px, py, &e);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, e.weights[s]);
        for (uint32_t o = 0; o < 4; ++o)
            if (o != s)
                TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, e.weights[o]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));
    }
    jce_anim_blend_tree_destroy(bt);
}

static void test_eval_center_of_symmetric_samples_is_equal(void)
{
    JceAnimBlendTree *bt = make_square_cartesian();
    JceAnimBlendTreeEval2D e = {0};

    /* Center of the symmetric square → all four weights equal (0.25). */
    jce_anim_blend_tree_eval_2d(bt, 0.0f, 0.0f, &e);
    TEST_ASSERT_EQUAL_UINT32(4u, e.count);
    for (uint32_t i = 0; i < 4; ++i)
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.25f, e.weights[i]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));
    jce_anim_blend_tree_destroy(bt);
}

static void test_eval_between_two_samples_interpolates(void)
{
    /* Three collinear samples along x; midpoint between two of them must
       split weight between exactly those two and sum to 1. */
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(3, false);
    (void)jce_anim_blend_tree_add_2d(bt, "a", 0.0f, 0.0f); /* 0 */
    (void)jce_anim_blend_tree_add_2d(bt, "b", 2.0f, 0.0f); /* 1 */
    (void)jce_anim_blend_tree_add_2d(bt, "c", 4.0f, 0.0f); /* 2 */

    JceAnimBlendTreeEval2D e = {0};
    jce_anim_blend_tree_eval_2d(bt, 1.0f, 0.0f, &e); /* midpoint a..b */

    /* Weights split between a and b; c gets ~0; sum is 1. */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.5f, e.weights[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.5f, e.weights[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, e.weights[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));

    /* Move toward b: b should outweigh a, both still > 0, sum 1. */
    jce_anim_blend_tree_eval_2d(bt, 1.5f, 0.0f, &e);
    TEST_ASSERT_TRUE(e.weights[1] > e.weights[0]);
    TEST_ASSERT_TRUE(e.weights[0] > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));
    jce_anim_blend_tree_destroy(bt);
}

static void test_eval_weights_never_negative_and_sum_one(void)
{
    JceAnimBlendTree *bt = make_square_cartesian();
    /* Sweep a grid of query points (incl. inside + outside the hull). */
    for (int iy = -3; iy <= 3; ++iy) {
        for (int ix = -3; ix <= 3; ++ix) {
            float qx = (float)ix * 0.8f;
            float qy = (float)iy * 0.8f;
            JceAnimBlendTreeEval2D e = {0};
            jce_anim_blend_tree_eval_2d(bt, qx, qy, &e);
            TEST_ASSERT_EQUAL_UINT32(4u, e.count);
            for (uint32_t i = 0; i < e.count; ++i)
                TEST_ASSERT_TRUE(e.weights[i] >= -1e-5f);
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, weight_sum(&e));
        }
    }
    jce_anim_blend_tree_destroy(bt);
}

static void test_eval_out_of_hull_clamps_sanely(void)
{
    JceAnimBlendTree *bt = make_square_cartesian();
    JceAnimBlendTreeEval2D e = {0};

    /* Far past the top-right corner → top-right (index 1) dominates. */
    jce_anim_blend_tree_eval_2d(bt, 100.0f, 100.0f, &e);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, e.weights[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));

    /* Far past the bottom-left corner → bottom-left (index 2) dominates. */
    jce_anim_blend_tree_eval_2d(bt, -100.0f, -100.0f, &e);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, e.weights[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));
    jce_anim_blend_tree_destroy(bt);
}

static void test_eval_single_sample_gets_all_weight(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(2, false);
    (void)jce_anim_blend_tree_add_2d(bt, "only", 3.0f, 7.0f);
    (void)jce_anim_blend_tree_set_clip(bt, "only", CLIP(0));

    JceAnimBlendTreeEval2D e = {0};
    jce_anim_blend_tree_eval_2d(bt, -5.0f, 2.0f, &e); /* anywhere */
    TEST_ASSERT_EQUAL_UINT32(1u, e.count);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, e.weights[0]);
    TEST_ASSERT_EQUAL_PTR(CLIP(0), e.clips[0]);
    jce_anim_blend_tree_destroy(bt);
}

/* ------------------------------------------------------------------ */
/* Directional evaluation                                              */
/* ------------------------------------------------------------------ */

/* Forward / back / left / right unit directions + a center idle sample. */
static JceAnimBlendTree *make_directional(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_2d(5, true);
    (void)jce_anim_blend_tree_add_2d(bt, "idle",  0.0f,  0.0f); /* 0 */
    (void)jce_anim_blend_tree_add_2d(bt, "fwd",   0.0f,  1.0f); /* 1 */
    (void)jce_anim_blend_tree_add_2d(bt, "back",  0.0f, -1.0f); /* 2 */
    (void)jce_anim_blend_tree_add_2d(bt, "left", -1.0f,  0.0f); /* 3 */
    (void)jce_anim_blend_tree_add_2d(bt, "right", 1.0f,  0.0f); /* 4 */
    return bt;
}

static void test_directional_at_sample_is_one(void)
{
    JceAnimBlendTree *bt = make_directional();
    /* Each non-center sample should dominate at its own position. */
    for (uint32_t s = 1; s < 5; ++s) {
        float px = 0.0f, py = 0.0f;
        jce_anim_blend_tree_position(bt, s, &px, &py);
        JceAnimBlendTreeEval2D e = {0};
        jce_anim_blend_tree_eval_2d(bt, px, py, &e);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, e.weights[s]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));
    }
    jce_anim_blend_tree_destroy(bt);
}

static void test_directional_no_opposite_bleed(void)
{
    JceAnimBlendTree *bt = make_directional();
    JceAnimBlendTreeEval2D e = {0};

    /* Query straight forward: "back" (the opposite direction) must get
       essentially zero weight even though it's collinear. */
    jce_anim_blend_tree_eval_2d(bt, 0.0f, 1.0f, &e);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, e.weights[2]); /* back */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, e.weights[1]); /* fwd  */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, weight_sum(&e));
    jce_anim_blend_tree_destroy(bt);
}

static void test_directional_diagonal_blends_two_neighbors(void)
{
    JceAnimBlendTree *bt = make_directional();
    JceAnimBlendTreeEval2D e = {0};

    /* Forward-right diagonal: fwd (1) and right (4) should share most of
       the weight; back (2) and left (3) get ~0; sum is 1. */
    jce_anim_blend_tree_eval_2d(bt, 0.7071f, 0.7071f, &e);
    TEST_ASSERT_TRUE(e.weights[1] > 0.1f);
    TEST_ASSERT_TRUE(e.weights[4] > 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 0.0f, e.weights[2]);
    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 0.0f, e.weights[3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, weight_sum(&e));
    jce_anim_blend_tree_destroy(bt);
}

static void test_eval_2d_on_null_is_safe(void)
{
    JceAnimBlendTreeEval2D e = {0};
    e.count = 99;
    jce_anim_blend_tree_eval_2d(NULL, 0.0f, 0.0f, &e);
    TEST_ASSERT_EQUAL_UINT32(0u, e.count);
    jce_anim_blend_tree_eval_2d(NULL, 0.0f, 0.0f, NULL); /* no crash */
    TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_2d_cartesian_mode);
    RUN_TEST(test_create_2d_directional_mode);
    RUN_TEST(test_add_2d_returns_sequential_indices);
    RUN_TEST(test_add_2d_full_returns_minus_one);
    RUN_TEST(test_add_2d_rejects_1d_tree);
    RUN_TEST(test_eval_at_sample_point_is_one_there_zero_elsewhere);
    RUN_TEST(test_eval_each_sample_point_dominates);
    RUN_TEST(test_eval_center_of_symmetric_samples_is_equal);
    RUN_TEST(test_eval_between_two_samples_interpolates);
    RUN_TEST(test_eval_weights_never_negative_and_sum_one);
    RUN_TEST(test_eval_out_of_hull_clamps_sanely);
    RUN_TEST(test_eval_single_sample_gets_all_weight);
    RUN_TEST(test_directional_at_sample_is_one);
    RUN_TEST(test_directional_no_opposite_bleed);
    RUN_TEST(test_directional_diagonal_blends_two_neighbors);
    RUN_TEST(test_eval_2d_on_null_is_safe);
    return UNITY_END();
}
