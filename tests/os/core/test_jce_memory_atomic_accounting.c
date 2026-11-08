/* Actual accounting owner, real mimalloc and real JCE worker/barrier services.
 * The isolated symbol names exclude unrelated thread-service allocations. */
#include "unity.h"
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_allocator.h>
#include <mimalloc.h>
#include <stdint.h>
#include <string.h>
#include "os/platform/jce_native_atomic_word_internal.h"

static bool observe_protocol;
static size_t observed_adds, observed_loads, observed_cas;
#ifndef JCE_MEMORY_PROTOCOL_CONTROL
static size_t observe_add(JceNativeAtomicWord *word, size_t value)
{
    if (observe_protocol) ++observed_adds;
    return jce_native_atomic_word_add(word, value);
}
#endif
static size_t observe_load(const JceNativeAtomicWord *word)
{
    if (observe_protocol) ++observed_loads;
    return jce_native_atomic_word_load(word);
}
static bool observe_compare_exchange(JceNativeAtomicWord *word,
    size_t expected, size_t desired)
{
    if (observe_protocol) ++observed_cas;
    return jce_native_atomic_word_compare_exchange(word, expected, desired);
}

#ifdef JCE_MEMORY_PROTOCOL_CONTROL
/* This control runs SINGLE-THREADED only. It restores plain counter RMW and
 * fails the atomic-protocol assertion even when its numerical answer is right.
 * Never execute concurrent plain-C accesses or rely on random lost updates. */
static size_t plain_word_add(JceNativeAtomicWord *word, size_t value)
{
    size_t previous = (size_t)word->value;
    word->value = (intptr_t)(previous + value);
    return previous;
}
#endif

#undef JCE_TRACY_ENABLED
#define JCE_TRACY_ENABLED 0
#define jce_native_atomic_word_add observe_add
#define jce_native_atomic_word_load observe_load
#define jce_native_atomic_word_compare_exchange observe_compare_exchange
#define jce_alloc_frame_delta account_frame_delta
#define jce_alloc_track_snapshot account_track_snapshot
#define jce__alloc_account_alloc account_alloc
#define jce__alloc_account_free account_free
#define jce__malloc_tracked account_malloc
#define jce__calloc_tracked account_calloc
#define jce__realloc_tracked account_realloc
#define jce__free_tracked account_release
#ifndef JCE_MEMORY_ACCOUNTING_OWNER
#define JCE_MEMORY_ACCOUNTING_OWNER "os/core/jce_memory.c"
#endif
#include JCE_MEMORY_ACCOUNTING_OWNER
#undef jce_native_atomic_word_add
#undef jce_native_atomic_word_load
#undef jce_native_atomic_word_compare_exchange

void setUp(void)
{
    /* Quiescent test boundary only; never reset a word under a live producer. */
    observe_protocol = false;
    observed_adds = observed_loads = observed_cas = 0;
#if JCE_ALLOC_TRACKING
    jce_native_atomic_word_store(&s_live_bytes, 0);
    jce_native_atomic_word_store(&s_live_count, 0);
    jce_native_atomic_word_store(&s_peak_bytes, 0);
    jce_native_atomic_word_store(&s_total_allocs, 0);
    jce_native_atomic_word_store(&s_total_frees, 0);
    for (int i = 0; i < JCE_ALLOC_TRACK_BUCKETS; ++i) {
        jce_native_atomic_word_store(&s_bkt_bytes[i], 0);
        jce_native_atomic_word_store(&s_bkt_count[i], 0);
    }
#endif
    jce_native_atomic_word_store(&s_pf_allocs, 0);
    jce_native_atomic_word_store(&s_pf_alloc_bytes, 0);
    s_pf_last_allocs = s_pf_last_bytes = 0;
}
void tearDown(void) { observe_protocol = false; }

static void test_shared_accounting_uses_atomic_protocol_without_recursive_allocations(void)
{
    observe_protocol = true;
    void *block = account_malloc(48);
    JceAllocTrack held, retired;
    bool held_ok = account_track_snapshot(&held);
    account_release(block);
    bool retired_ok = account_track_snapshot(&retired);
    uint64_t allocations = 0, bytes = 0;
    account_frame_delta(&allocations, &bytes);
    observe_protocol = false;
    TEST_ASSERT_NOT_NULL(block);
#if JCE_ALLOC_TRACKING
    TEST_ASSERT_TRUE(held_ok && retired_ok);
    TEST_ASSERT_EQUAL_UINT64(1, held.live_count);
    TEST_ASSERT_TRUE(held.live_bytes >= 48);
    TEST_ASSERT_EQUAL_UINT64(0, retired.live_count);
    TEST_ASSERT_EQUAL_UINT64(0, retired.live_bytes);
    TEST_ASSERT_EQUAL_UINT64(1, retired.total_allocs);
    TEST_ASSERT_EQUAL_UINT64(1, retired.total_frees);
#else
    TEST_ASSERT_FALSE(held_ok || retired_ok);
    TEST_ASSERT_EQUAL_UINT64(0, held.live_count);
    TEST_ASSERT_EQUAL_UINT64(0, held.live_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, retired.total_allocs);
    TEST_ASSERT_EQUAL_UINT64(0, retired.total_frees);
#endif
    TEST_ASSERT_EQUAL_UINT64(1, allocations);
    TEST_ASSERT_EQUAL_UINT64(48, bytes);
    /* Forwarding observation ports call the actual native adapter. The old
     * plain RMW copy deterministically records zero RMW calls, without UB. */
    TEST_ASSERT_TRUE_MESSAGE(observed_loads > 0, "shared observations bypass atomic loads");
#if JCE_ALLOC_TRACKING
    TEST_ASSERT_TRUE_MESSAGE(observed_adds >= 12, "shared writes bypass native atomic RMW");
    TEST_ASSERT_TRUE_MESSAGE(observed_cas > 0, "peak bypasses native compare-exchange");
#else
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(2, observed_adds, "Release always-on RMW protocol missing");
    TEST_ASSERT_EQUAL_UINT64(0, observed_cas);
#endif
}

#if !defined(JCE_MEMORY_PROTOCOL_CONTROL)
#if !JCE_PLATFORM_WEB
enum { ACCOUNT_WORKERS = 4, ACCOUNT_BATCHES = 3, ACCOUNT_BLOCKS = 64 };
typedef struct AccountWorker {
    JceSemaphore *go, *held, *release, *done;
    JceThread *thread;
    void *blocks[ACCOUNT_BLOCKS];
    size_t requested;
    bool stop, failed;
} AccountWorker;

static void account_worker(void *argument)
{
    AccountWorker *worker = (AccountWorker *)argument;
    for (int batch = 0; batch < ACCOUNT_BATCHES; ++batch) {
        jce_semaphore_wait(worker->go);
        if (worker->stop) return;
        for (int i = 0; i < ACCOUNT_BLOCKS; ++i) {
            worker->blocks[i] = account_malloc(worker->requested);
            if (!worker->blocks[i]) worker->failed = true;
        }
        jce_semaphore_signal(worker->held);
        jce_semaphore_wait(worker->release);
        for (int i = 0; i < ACCOUNT_BLOCKS; ++i) account_release(worker->blocks[i]);
        jce_semaphore_signal(worker->done);
    }
}

static void test_real_workers_join_and_single_consumer_batch_deltas(void)
{
    AccountWorker workers[ACCOUNT_WORKERS];
    memset(workers, 0, sizeof workers);
    bool prepared = true, started = true, valid = true;
    uint64_t total_allocations = 0, total_bytes = 0;
    size_t expected_bytes = 0;
    int created = 0;
    for (int i = 0; i < ACCOUNT_WORKERS; ++i) {
        workers[i].go = jce_semaphore_create(0);
        workers[i].held = jce_semaphore_create(0);
        workers[i].release = jce_semaphore_create(0);
        workers[i].done = jce_semaphore_create(0);
        workers[i].requested = (size_t)(63 + i);
        expected_bytes += workers[i].requested * ACCOUNT_BLOCKS;
        prepared = prepared && workers[i].go && workers[i].held &&
                   workers[i].release && workers[i].done;
    }
    if (prepared) {
        for (int i = 0; i < ACCOUNT_WORKERS; ++i) {
            workers[i].thread = jce_thread_create(account_worker, &workers[i], "account-worker");
            if (!workers[i].thread) { started = false; break; }
            ++created;
        }
    }
    if (prepared && started) {
        for (int batch = 0; batch < ACCOUNT_BATCHES; ++batch) {
            for (int i = 0; i < ACCOUNT_WORKERS; ++i) jce_semaphore_signal(workers[i].go);
            for (int i = 0; i < ACCOUNT_WORKERS; ++i) jce_semaphore_wait(workers[i].held);
            size_t usable = 0;
            for (int i = 0; i < ACCOUNT_WORKERS; ++i) {
                valid = valid && !workers[i].failed;
                for (int j = 0; j < ACCOUNT_BLOCKS; ++j)
                    if (workers[i].blocks[j]) usable += mi_usable_size(workers[i].blocks[j]);
            }
            JceAllocTrack held;
            bool held_observed = account_track_snapshot(&held);
#if JCE_ALLOC_TRACKING
            valid = held_observed && valid;
            valid = valid && held.live_bytes == usable && held.peak_bytes >= usable &&
                held.live_count == ACCOUNT_WORKERS * ACCOUNT_BLOCKS &&
                held.total_allocs == (uint64_t)(batch + 1) * ACCOUNT_WORKERS * ACCOUNT_BLOCKS;
#else
            valid = valid && !held_observed && held.live_bytes == 0 &&
                held.live_count == 0 && held.peak_bytes == 0 && held.total_allocs == 0;
            (void)usable;
#endif
            uint64_t allocations = 0, bytes = 0;
            account_frame_delta(&allocations, &bytes); /* the sole consumer */
            total_allocations += allocations;
            total_bytes += bytes;
            valid = valid && allocations == ACCOUNT_WORKERS * ACCOUNT_BLOCKS && bytes == expected_bytes;
            for (int i = 0; i < ACCOUNT_WORKERS; ++i) jce_semaphore_signal(workers[i].release);
            for (int i = 0; i < ACCOUNT_WORKERS; ++i) jce_semaphore_wait(workers[i].done);
            JceAllocTrack retired;
            bool retired_observed = account_track_snapshot(&retired);
#if JCE_ALLOC_TRACKING
            valid = retired_observed && valid;
            valid = valid && retired.live_bytes == 0 && retired.live_count == 0 &&
                retired.total_frees == (uint64_t)(batch + 1) * ACCOUNT_WORKERS * ACCOUNT_BLOCKS;
#else
            valid = valid && !retired_observed && retired.live_bytes == 0 &&
                retired.live_count == 0 && retired.total_allocs == 0 && retired.total_frees == 0;
#endif
            for (int bucket = 0; bucket < JCE_ALLOC_TRACK_BUCKETS; ++bucket)
                valid = valid && retired.bucket_bytes[bucket] == 0 && retired.bucket_count[bucket] == 0;
            account_frame_delta(&allocations, &bytes);
            valid = valid && allocations == 0 && bytes == 0;
        }
    } else {
        /* A partial thread creation still has one explicit release/join owner. */
        for (int i = 0; i < created; ++i) {
            workers[i].stop = true;
            jce_semaphore_signal(workers[i].go);
        }
    }
    for (int i = 0; i < created; ++i) jce_thread_join(workers[i].thread);
    for (int i = 0; i < ACCOUNT_WORKERS; ++i) {
        jce_semaphore_destroy(workers[i].go);
        jce_semaphore_destroy(workers[i].held);
        jce_semaphore_destroy(workers[i].release);
        jce_semaphore_destroy(workers[i].done);
    }
    JceAllocTrack joined;
    bool observed = account_track_snapshot(&joined);
    TEST_ASSERT_TRUE(prepared && started);
    TEST_ASSERT_TRUE(valid);
#if JCE_ALLOC_TRACKING
    TEST_ASSERT_TRUE(observed);
#else
    TEST_ASSERT_FALSE(observed);
#endif
    TEST_ASSERT_EQUAL_UINT64(0, joined.live_count);
    TEST_ASSERT_EQUAL_UINT64(0, joined.live_bytes);
    TEST_ASSERT_EQUAL_UINT64(ACCOUNT_BATCHES * ACCOUNT_WORKERS * ACCOUNT_BLOCKS, total_allocations);
    TEST_ASSERT_EQUAL_UINT64(ACCOUNT_BATCHES * expected_bytes, total_bytes);
}
#else
static void test_real_workers_join_and_single_consumer_batch_deltas(void)
{ TEST_IGNORE_MESSAGE("Current Web execution is cooperative; no real worker qualification"); }
#endif
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shared_accounting_uses_atomic_protocol_without_recursive_allocations);
#ifndef JCE_MEMORY_PROTOCOL_CONTROL
    RUN_TEST(test_real_workers_join_and_single_consumer_batch_deltas);
#endif
    return UNITY_END();
}
