/* test_jce_thread_parallel_for.c
 *
 * Replaces test_jce_jobs.c: the jce_jobs facade was retired onto the enkiTS
 * JceThreadPool, so its API no longer exists.
 *
 * The property under test is the one the retirement nearly broke.  The obvious
 * migration — calling jce_thread_pool_submit_range(pool, fn, arg, count, chunk)
 * — is WRONG, because enkiTS chooses its own partition boundaries and `chunk`
 * is only a MINIMUM.  Two consumers derive a per-chunk slot index from `begin`
 * and write it single-writer:
 *
 *   jce_render_queue.c  rq_flush_chunk : ci = begin / per;  stats[ci] = ...
 *   jce_scene_renderer.c sr_ecull_hit_range : chunk_id = begin / CHUNK;
 *                                             ecull_key_parts[chunk_id] = ...
 *
 * If a partition ever started at a non-multiple of `chunk`, two partitions
 * would collapse onto one slot (one silently clobbering the other) or one
 * partition would straddle two slots and write misses into the wrong segment —
 * dropped draw calls and unresolved cull entities, with nothing to catch it.
 *
 * jce_thread_pool_parallel_for therefore guarantees `begin % chunk == 0` and
 * that every chunk index is visited exactly once.  These tests assert both.
 */

#include <jce/os/core/jce_thread.h>

#include "unity.h"

#include <stdint.h>
#include <string.h>

#define MAX_N 4096

typedef struct {
    uint32_t chunk;
    /* Per-INDEX visit count: proves full, non-overlapping coverage. */
    uint8_t  visits[MAX_N];
    /* Per-CHUNK visit count: proves one partition per chunk slot — the
     * single-writer property the render queue and ecull depend on. */
    uint8_t  chunk_visits[MAX_N];
    /* Set if any partition began at a non-multiple of chunk. */
    int      misaligned;
    uint32_t count;
} PForCheck;

static void check_range(uint32_t begin, uint32_t end, void *arg)
{
    PForCheck *c = (PForCheck *)arg;

    if (c->chunk && (begin % c->chunk) != 0)
        c->misaligned = 1;          /* the invariant both consumers rely on */

    if (c->chunk) {
        const uint32_t ci = begin / c->chunk;
        if (ci < MAX_N) c->chunk_visits[ci]++;
    }
    for (uint32_t i = begin; i < end && i < MAX_N; ++i)
        c->visits[i]++;
}

static void run_case(uint32_t count, uint32_t chunk)
{
    JceThreadPool *pool = jce_thread_pool_shared();
    TEST_ASSERT_NOT_NULL(pool);

    PForCheck c;
    memset(&c, 0, sizeof c);
    c.chunk = chunk;
    c.count = count;

    jce_thread_pool_parallel_for(pool, count, chunk, check_range, &c);

    char msg[128];
    snprintf(msg, sizeof msg, "count=%u chunk=%u: partition began mid-chunk",
             count, chunk);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, c.misaligned, msg);

    /* Every index covered exactly once. */
    for (uint32_t i = 0; i < count && i < MAX_N; ++i) {
        snprintf(msg, sizeof msg, "count=%u chunk=%u: index %u visited %u times",
                 count, chunk, i, c.visits[i]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, c.visits[i], msg);
    }
    /* Nothing past the end. */
    for (uint32_t i = count; i < MAX_N; ++i)
        TEST_ASSERT_EQUAL_UINT8(0, c.visits[i]);

    /* Every chunk slot written exactly once — the single-writer guarantee. */
    const uint32_t n_chunks = chunk ? ((count + chunk - 1u) / chunk) : 0u;
    for (uint32_t ci = 0; ci < n_chunks && ci < MAX_N; ++ci) {
        snprintf(msg, sizeof msg,
                 "count=%u chunk=%u: chunk slot %u written %u times",
                 count, chunk, ci, c.chunk_visits[ci]);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, c.chunk_visits[ci], msg);
    }
}

void setUp(void)    {}
void tearDown(void) {}

/* Exact multiples, remainders, one chunk, and a chunk larger than the count —
 * the four shapes the nine engine call sites actually produce. */
static void test_partitions_are_chunk_aligned(void)
{
    run_case(1024, 64);     /* exact multiple                        */
    run_case(1000, 64);     /* ragged tail                           */
    run_case(64,   64);     /* exactly one chunk (serial fast path)  */
    run_case(10,   64);     /* chunk > count                         */
    run_case(1,    1);      /* degenerate                            */
    run_case(4096, 1);      /* max chunks, min grain                 */
}

/* count == 0 must be a no-op, not a hang or a crash. */
static void test_zero_count_is_a_noop(void)
{
    JceThreadPool *pool = jce_thread_pool_shared();
    TEST_ASSERT_NOT_NULL(pool);

    PForCheck c;
    memset(&c, 0, sizeof c);
    c.chunk = 32;
    jce_thread_pool_parallel_for(pool, 0, 32, check_range, &c);

    for (uint32_t i = 0; i < MAX_N; ++i)
        TEST_ASSERT_EQUAL_UINT8(0, c.visits[i]);
    TEST_ASSERT_EQUAL_INT(0, c.misaligned);
}

/* The pool must still be usable afterwards (the wait must actually join). */
static void test_repeated_calls_are_stable(void)
{
    for (int rep = 0; rep < 8; ++rep)
        run_case(777, 32);

    JceThreadPool *pool = jce_thread_pool_shared();
    TEST_ASSERT_TRUE_MESSAGE(jce_thread_pool_worker_count(pool) >= 0,
                             "shared pool unusable after repeated parallel_for");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_partitions_are_chunk_aligned);
    RUN_TEST(test_zero_count_is_a_noop);
    RUN_TEST(test_repeated_calls_are_stable);
    return UNITY_END();
}
