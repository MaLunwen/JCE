/*
 * test_jce_graph_astar.c — Unit tests for jce_graph_astar.h (L3 AI).
 *
 * Builds tiny ad-hoc graphs (linear chain, diamond, disconnected) via
 * caller-supplied neighbor / heuristic callbacks and checks that A*
 * returns the optimal path, correct cost, and gracefully reports
 * "no path" when nodes are unreachable.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_graph_astar.h>

#include <stdint.h>
#include <stdlib.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Graph 1: linear chain 0 - 1 - 2 - 3 - 4 (edge cost 1).             */
/* ------------------------------------------------------------------ */

static uint32_t chain_neighbors(uint32_t node, uint32_t *out_ids,
                                float *out_costs, uint32_t cap, void *u)
{
    (void)u;
    uint32_t n = 0;
    if (node > 0 && n < cap) { out_ids[n] = node - 1; out_costs[n] = 1.0f; ++n; }
    if (node < 4 && n < cap) { out_ids[n] = node + 1; out_costs[n] = 1.0f; ++n; }
    return n;
}

static float manhattan_heuristic(uint32_t node, uint32_t goal, void *u)
{
    (void)u;
    return (float)((node > goal) ? (node - goal) : (goal - node));
}

/* ------------------------------------------------------------------ */
/* Graph 2: diamond                                                    */
/*           0                                                          */
/*          / \                                                         */
/*         1   2   ← cost(0→1)=1, cost(0→2)=10                          */
/*          \ /                                                         */
/*           3                                                          */
/* ------------------------------------------------------------------ */

static uint32_t diamond_neighbors(uint32_t node, uint32_t *out_ids,
                                  float *out_costs, uint32_t cap, void *u)
{
    (void)u;
    uint32_t n = 0;
    switch (node) {
    case 0:
        if (n < cap) { out_ids[n] = 1; out_costs[n] = 1.0f;  ++n; }
        if (n < cap) { out_ids[n] = 2; out_costs[n] = 10.0f; ++n; }
        break;
    case 1:
        if (n < cap) { out_ids[n] = 3; out_costs[n] = 1.0f; ++n; }
        break;
    case 2:
        if (n < cap) { out_ids[n] = 3; out_costs[n] = 1.0f; ++n; }
        break;
    case 3:
        break;
    default: break;
    }
    return n;
}

static float zero_heuristic(uint32_t node, uint32_t goal, void *u)
{
    (void)node; (void)goal; (void)u;
    return 0.0f;
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    TEST_ASSERT_NOT_NULL(a);
    jce_graph_astar_destroy(a);
}

static void test_chain_finds_optimal_path(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    jce_graph_astar_set_callbacks(a, chain_neighbors, manhattan_heuristic, NULL);

    uint32_t path[16];
    uint32_t count = 0;
    bool ok = jce_graph_astar_search(a, 0, 4, path, 16, &count);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(5u, count);
    TEST_ASSERT_EQUAL_UINT32(0u, path[0]);
    TEST_ASSERT_EQUAL_UINT32(4u, path[count - 1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 4.0f, jce_graph_astar_last_cost(a));
    jce_graph_astar_destroy(a);
}

static void test_chain_same_start_goal_returns_singleton(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    jce_graph_astar_set_callbacks(a, chain_neighbors, manhattan_heuristic, NULL);

    uint32_t path[4];
    uint32_t count = 0;
    bool ok = jce_graph_astar_search(a, 2, 2, path, 4, &count);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(1u, count);
    TEST_ASSERT_EQUAL_UINT32(2u, path[0]);
    jce_graph_astar_destroy(a);
}

static void test_diamond_picks_cheaper_branch(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    jce_graph_astar_set_callbacks(a, diamond_neighbors, zero_heuristic, NULL);

    uint32_t path[8];
    uint32_t count = 0;
    bool ok = jce_graph_astar_search(a, 0, 3, path, 8, &count);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(3u, count);
    TEST_ASSERT_EQUAL_UINT32(0u, path[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, path[1]);  /* cheap branch via 1, not 2. */
    TEST_ASSERT_EQUAL_UINT32(3u, path[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, jce_graph_astar_last_cost(a));
    jce_graph_astar_destroy(a);
}

static void test_unreachable_goal_returns_false(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    jce_graph_astar_set_callbacks(a, diamond_neighbors, zero_heuristic, NULL);

    uint32_t path[8];
    uint32_t count = 0;
    /* Node 99 isn't in the diamond — no neighbors lead to it. */
    bool ok = jce_graph_astar_search(a, 0, 99, path, 8, &count);
    TEST_ASSERT_FALSE(ok);
    jce_graph_astar_destroy(a);
}

static void test_search_reusable_across_calls(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    jce_graph_astar_set_callbacks(a, chain_neighbors, manhattan_heuristic, NULL);

    uint32_t path[16];
    uint32_t count = 0;
    (void)jce_graph_astar_search(a, 0, 4, path, 16, &count);
    TEST_ASSERT_EQUAL_UINT32(5u, count);

    /* Re-run with different endpoints; internal state must reset. */
    count = 0;
    bool ok = jce_graph_astar_search(a, 1, 3, path, 16, &count);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(3u, count);
    TEST_ASSERT_EQUAL_UINT32(1u, path[0]);
    TEST_ASSERT_EQUAL_UINT32(3u, path[2]);
    jce_graph_astar_destroy(a);
}

static void test_nodes_expanded_counter_progresses(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    jce_graph_astar_set_callbacks(a, chain_neighbors, manhattan_heuristic, NULL);

    uint32_t path[16];
    uint32_t count = 0;
    (void)jce_graph_astar_search(a, 0, 4, path, 16, &count);
    TEST_ASSERT_TRUE(jce_graph_astar_last_nodes_expanded(a) >= 1u);
    jce_graph_astar_destroy(a);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_chain_finds_optimal_path);
    RUN_TEST(test_chain_same_start_goal_returns_singleton);
    RUN_TEST(test_diamond_picks_cheaper_branch);
    RUN_TEST(test_unreachable_goal_returns_false);
    RUN_TEST(test_search_reusable_across_calls);
    RUN_TEST(test_nodes_expanded_counter_progresses);
    return UNITY_END();
}
