/*
 * test_jce_anim_blend_tree.c — Unit tests for jce_anim_blend_tree.h (L3).
 *
 * 1D blend tree returns two adjacent entries bracketing the input
 * parameter, with normalised blend weights summing to 1.  Clip pointers
 * are bound by name; the tree itself is pure logic.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_anim_blend_tree.h>

#include <stddef.h>
#include <stdint.h>

void setUp(void)    {}
void tearDown(void) {}

/* JceAnimClip is an opaque type; tests only need stable pointer
   identity, so use dummy storage and reinterpret as JceAnimClip*. */
static char g_clip_a_storage;
static char g_clip_b_storage;
static char g_clip_c_storage;

#define CLIP_A ((const JceAnimClip *)&g_clip_a_storage)
#define CLIP_B ((const JceAnimClip *)&g_clip_b_storage)
#define CLIP_C ((const JceAnimClip *)&g_clip_c_storage)

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    TEST_ASSERT_NOT_NULL(bt);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_anim_blend_tree_count(bt));
    jce_anim_blend_tree_destroy(bt);
}

static void test_add_returns_sequential_indices(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    TEST_ASSERT_EQUAL_INT(0, jce_anim_blend_tree_add(bt, "idle", 0.0f));
    TEST_ASSERT_EQUAL_INT(1, jce_anim_blend_tree_add(bt, "walk", 2.0f));
    TEST_ASSERT_EQUAL_INT(2, jce_anim_blend_tree_add(bt, "run",  6.0f));
    TEST_ASSERT_EQUAL_UINT32(3u, jce_anim_blend_tree_count(bt));
    jce_anim_blend_tree_destroy(bt);
}

static void test_add_full_returns_minus_one(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(2);
    (void)jce_anim_blend_tree_add(bt, "a", 0.0f);
    (void)jce_anim_blend_tree_add(bt, "b", 1.0f);
    TEST_ASSERT_EQUAL_INT(-1, jce_anim_blend_tree_add(bt, "c", 2.0f));
    jce_anim_blend_tree_destroy(bt);
}

static void test_set_clip_binds_pointer(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    (void)jce_anim_blend_tree_add(bt, "idle", 0.0f);
    TEST_ASSERT_TRUE(jce_anim_blend_tree_set_clip(bt, "idle", CLIP_A));
    TEST_ASSERT_FALSE(jce_anim_blend_tree_set_clip(bt, "ghost", CLIP_A));
    jce_anim_blend_tree_destroy(bt);
}

/* ------------------------------------------------------------------ */
/* Evaluation                                                          */
/* ------------------------------------------------------------------ */

static void test_evaluate_midway_gives_half_weights(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    (void)jce_anim_blend_tree_add(bt, "a", 0.0f);
    (void)jce_anim_blend_tree_add(bt, "b", 2.0f);
    (void)jce_anim_blend_tree_set_clip(bt, "a", CLIP_A);
    (void)jce_anim_blend_tree_set_clip(bt, "b", CLIP_B);

    JceAnimBlendTreeEval out = {0};
    jce_anim_blend_tree_evaluate(bt, 1.0f, &out);

    TEST_ASSERT_EQUAL_PTR(CLIP_A, out.clip_a);
    TEST_ASSERT_EQUAL_PTR(CLIP_B, out.clip_b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, out.weight_a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, out.weight_b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, out.weight_a + out.weight_b);
    jce_anim_blend_tree_destroy(bt);
}

static void test_evaluate_below_min_clamps_to_first(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    (void)jce_anim_blend_tree_add(bt, "a", 1.0f);
    (void)jce_anim_blend_tree_add(bt, "b", 3.0f);
    (void)jce_anim_blend_tree_set_clip(bt, "a", CLIP_A);
    (void)jce_anim_blend_tree_set_clip(bt, "b", CLIP_B);

    JceAnimBlendTreeEval out = {0};
    jce_anim_blend_tree_evaluate(bt, -10.0f, &out);

    TEST_ASSERT_EQUAL_PTR(CLIP_A, out.clip_a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, out.weight_a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, out.weight_b);
    jce_anim_blend_tree_destroy(bt);
}

static void test_evaluate_above_max_clamps_to_last(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    (void)jce_anim_blend_tree_add(bt, "a", 1.0f);
    (void)jce_anim_blend_tree_add(bt, "b", 3.0f);
    (void)jce_anim_blend_tree_set_clip(bt, "a", CLIP_A);
    (void)jce_anim_blend_tree_set_clip(bt, "b", CLIP_B);

    JceAnimBlendTreeEval out = {0};
    jce_anim_blend_tree_evaluate(bt, 100.0f, &out);

    TEST_ASSERT_EQUAL_PTR(CLIP_B, out.clip_a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, out.weight_a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, out.weight_b);
    jce_anim_blend_tree_destroy(bt);
}

static void test_evaluate_unsorted_input_still_correct(void)
{
    /* Add entries out of order — first evaluate() must sort and still
       produce correct bracketing. */
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    (void)jce_anim_blend_tree_add(bt, "c", 4.0f);
    (void)jce_anim_blend_tree_add(bt, "a", 0.0f);
    (void)jce_anim_blend_tree_add(bt, "b", 2.0f);
    (void)jce_anim_blend_tree_set_clip(bt, "a", CLIP_A);
    (void)jce_anim_blend_tree_set_clip(bt, "b", CLIP_B);
    (void)jce_anim_blend_tree_set_clip(bt, "c", CLIP_C);

    JceAnimBlendTreeEval out = {0};
    jce_anim_blend_tree_evaluate(bt, 3.0f, &out);
    /* 3.0 sits between thresholds 2.0 (b) and 4.0 (c). */
    TEST_ASSERT_EQUAL_PTR(CLIP_B, out.clip_a);
    TEST_ASSERT_EQUAL_PTR(CLIP_C, out.clip_b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, out.weight_a);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, out.weight_b);
    jce_anim_blend_tree_destroy(bt);
}

static void test_query_helpers(void)
{
    JceAnimBlendTree *bt = jce_anim_blend_tree_create_1d(4);
    (void)jce_anim_blend_tree_add(bt, "idle", 0.5f);
    TEST_ASSERT_EQUAL_STRING("idle", jce_anim_blend_tree_name(bt, 0));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f,
                             jce_anim_blend_tree_threshold(bt, 0));
    jce_anim_blend_tree_destroy(bt);
}

static void test_destroy_null_is_safe(void)
{
    jce_anim_blend_tree_destroy(NULL);
    TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_add_returns_sequential_indices);
    RUN_TEST(test_add_full_returns_minus_one);
    RUN_TEST(test_set_clip_binds_pointer);
    RUN_TEST(test_evaluate_midway_gives_half_weights);
    RUN_TEST(test_evaluate_below_min_clamps_to_first);
    RUN_TEST(test_evaluate_above_max_clamps_to_last);
    RUN_TEST(test_evaluate_unsorted_input_still_correct);
    RUN_TEST(test_query_helpers);
    RUN_TEST(test_destroy_null_is_safe);
    return UNITY_END();
}
