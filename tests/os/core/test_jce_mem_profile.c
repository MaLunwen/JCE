/* Voluntary tagged peak semantics and concurrent accounting, without GUI. */
#include "unity.h"
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_thread.h>

#define TAG_A ((JceMemTag)JCE_MEM_TAG_USER_BASE)
#define TAG_B ((JceMemTag)(JCE_MEM_TAG_USER_BASE + 1))

void setUp(void) { jce_mem_profile_reset_peaks(); }
void tearDown(void) { }

static void test_staggered_and_overlapping_peaks(void)
{
    uint64_t baseline, current, peak, legacy;
    jce_mem_profile_get_aggregate_totals(&baseline, NULL);
    jce_mem_profile_record_alloc(TAG_A, 100);
    jce_mem_profile_record_free(TAG_A, 100);
    jce_mem_profile_record_alloc(TAG_B, 200);
    jce_mem_profile_get_aggregate_totals(&current, &peak);
    jce_mem_profile_get_total(NULL, &legacy);
    TEST_ASSERT_EQUAL_UINT64(baseline + 200, current);
    TEST_ASSERT_EQUAL_UINT64(baseline + 200, peak);
    TEST_ASSERT_EQUAL_UINT64(baseline + 300, legacy);
    jce_mem_profile_record_alloc(TAG_A, 100);
    jce_mem_profile_get_aggregate_totals(NULL, &peak);
    TEST_ASSERT_EQUAL_UINT64(baseline + 300, peak);
    jce_mem_profile_record_free(TAG_A, 999); /* Same clamp feeds both totals. */
    jce_mem_profile_record_free(TAG_B, 200);
    jce_mem_profile_reset_peaks();
    jce_mem_profile_get_aggregate_totals(&current, &peak);
    TEST_ASSERT_EQUAL_UINT64(baseline, current);
    TEST_ASSERT_EQUAL_UINT64(baseline, peak);
    jce_mem_profile_record_alloc((JceMemTag)-1, 1000);
    jce_mem_profile_record_alloc(TAG_A, 0);
    jce_mem_profile_get_aggregate_totals(NULL, NULL);
    jce_mem_profile_get_aggregate_totals(&current, NULL);
    TEST_ASSERT_EQUAL_UINT64(baseline, current);
}

typedef struct WorkerState {
    JceSemaphore *ready;
    JceSemaphore *release;
} WorkerState;

static void worker(void *arg)
{
    WorkerState *state = arg;
    jce_mem_profile_record_alloc(TAG_A, 100);
    jce_semaphore_signal(state->ready);
    jce_semaphore_wait(state->release);
    jce_mem_profile_record_free(TAG_A, 100);
}

static void test_workers_hold_a_real_simultaneous_peak(void)
{
    WorkerState state = {jce_semaphore_create(0), jce_semaphore_create(0)};
    JceThread *threads[4];
    uint64_t baseline, current, peak;
    TEST_ASSERT_NOT_NULL(state.ready);
    TEST_ASSERT_NOT_NULL(state.release);
    jce_mem_profile_get_aggregate_totals(&baseline, NULL);
    for (int i = 0; i < 4; ++i) {
        threads[i] = jce_thread_create(worker, &state, "mem-profile-test");
        TEST_ASSERT_NOT_NULL(threads[i]);
    }
    for (int i = 0; i < 4; ++i) jce_semaphore_wait(state.ready);
    jce_mem_profile_get_aggregate_totals(&current, &peak);
    TEST_ASSERT_EQUAL_UINT64(baseline + 400, current);
    TEST_ASSERT_EQUAL_UINT64(baseline + 400, peak);
    for (int i = 0; i < 4; ++i) jce_semaphore_signal(state.release);
    for (int i = 0; i < 4; ++i) jce_thread_join(threads[i]);
    jce_mem_profile_get_aggregate_totals(&current, NULL);
    TEST_ASSERT_EQUAL_UINT64(baseline, current);
    jce_semaphore_destroy(state.ready);
    jce_semaphore_destroy(state.release);
}

static void test_heap_unavailable_is_not_process_commit(void)
{
    JceProcessMemStats stats;
    TEST_ASSERT_FALSE(jce_mem_profile_get_process_stats(NULL));
    TEST_ASSERT_TRUE(jce_mem_profile_get_process_stats(&stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.mimalloc_reserved_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, stats.mimalloc_committed_bytes);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_staggered_and_overlapping_peaks);
    RUN_TEST(test_workers_hold_a_real_simultaneous_peak);
    RUN_TEST(test_heap_unavailable_is_not_process_commit);
    return UNITY_END();
}
