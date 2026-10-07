/* Pure decision regression; no window, engine initialization or rendering. */
#include "../../editor/src/panels/jce_hierarchy_capacity.h"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
TEST_CASE("hierarchy capacity grows without truncation and rejects invalid budgets")
{
    const size_t budget = 64u * 1024u * 1024u;
    int capacity = -1;
    CHECK(jce_hierarchy_capacity(0, 0, 0, 20, budget, &capacity));
    CHECK(capacity == 0);
    CHECK(jce_hierarchy_capacity(0, 10, 0, 20, budget, &capacity));
    CHECK(capacity == 256);
    CHECK(jce_hierarchy_capacity(capacity, 10, 2, 20, budget, &capacity));
    CHECK(capacity == 256);
    CHECK(jce_hierarchy_capacity(capacity, 200000, 500, 20, budget, &capacity));
    CHECK(capacity >= 200500);
    CHECK(capacity > 32768);
    int retained = capacity;
    CHECK(jce_hierarchy_capacity(capacity, 0, 0, 20, budget, &capacity));
    CHECK(capacity == retained);
    CHECK(jce_hierarchy_capacity(capacity, 10000, 0, 20, budget, &capacity));
    CHECK(capacity == retained);
    CHECK(!jce_hierarchy_capacity(capacity, INT_MAX, 1, 20, budget, &capacity));
    CHECK(!jce_hierarchy_capacity(capacity, -1, 0, 20, budget, &capacity));
    CHECK(!jce_hierarchy_capacity(capacity, 4000000, 0, 20, budget, &capacity));
    CHECK(!jce_hierarchy_capacity(0, 1, 0, 0, budget, &capacity));
    CHECK(jce_hierarchy_capacity(0, 2, 0, 20, 40, &capacity));
    CHECK(capacity == 2);
}

#include <cstdlib>
static int allocation_call, fail_call, live_blocks;
static void *fault_alloc(size_t bytes) {
    if (++allocation_call == fail_call) return nullptr;
    void *result = std::malloc(bytes);
    if (result) ++live_blocks;
    return result;
}
static void fault_free(void *value) {
    if (value) { --live_blocks; std::free(value); }
}
TEST_CASE("hierarchy transactional replacement preserves storage on every failed allocation")
{
    for (int fault = 1; fault <= 3; ++fault) {
        allocation_call = 0; fail_call = 0; live_blocks = 0;
        void *rows = fault_alloc(10), *order = fault_alloc(10), *roots = fault_alloc(10);
        void *old_rows = rows, *old_order = order, *old_roots = roots;
        allocation_call = 0; fail_call = fault;
        CHECK_FALSE(jce_hierarchy_replace(&rows, &order, &roots, 100, 100,
                                          fault_alloc, fault_free));
        CHECK(rows == old_rows); CHECK(order == old_order); CHECK(roots == old_roots);
        CHECK(live_blocks == 3);
        allocation_call = 0; fail_call = 0;
        CHECK(jce_hierarchy_replace(&rows, &order, &roots, 100, 100, fault_alloc, fault_free));
        CHECK(live_blocks == 3);
        fault_free(rows); fault_free(order); fault_free(roots);
        CHECK(live_blocks == 0);
    }
}
