/* Compile the real ring owner with renamed symbols, not a mirror reader. */
#include "unity.h"
#include <stdio.h>
#include <SDL3/SDL.h>
static bool inject_enable_exhaustion;
static unsigned enable_exhaustion_hits;
static int trace_enabled_store_port(SDL_AtomicInt *value, int desired);
#define SDL_SetAtomicInt trace_enabled_store_port
#define jce_trace_suppress_begin controlled_jce_trace_suppress_begin
#define jce_trace_suppress_end controlled_jce_trace_suppress_end
#define jce_trace_set_enabled controlled_jce_trace_set_enabled
#define jce_trace_enabled controlled_jce_trace_enabled
#define jce_trace_reset controlled_jce_trace_reset
#define jce_trace_next_id controlled_jce_trace_next_id
#define jce_trace_task_current_id controlled_jce_trace_task_current_id
#define jce_trace_task_restore_id controlled_jce_trace_task_restore_id
#define jce_trace_thread_register controlled_jce_trace_thread_register
#define jce_trace_thread_unregister controlled_jce_trace_thread_unregister
#define jce_trace_task_submit controlled_jce_trace_task_submit
#define jce_trace_task_begin controlled_jce_trace_task_begin
#define jce_trace_task_end controlled_jce_trace_task_end
#define jce_trace_wait_begin controlled_jce_trace_wait_begin
#define jce_trace_wait_end controlled_jce_trace_wait_end
#define jce_trace_frame_mark controlled_jce_trace_frame_mark
#define jce_trace_counter controlled_jce_trace_counter
#define jce_trace_counter_scoped_internal controlled_jce_trace_counter_scoped_internal
#define jce_trace_counter_ring_internal controlled_jce_trace_counter_ring_internal
#define jce_trace_cursor_init controlled_jce_trace_cursor_init
#define jce_trace_read controlled_jce_trace_read
#define jce_trace_read_observed_internal controlled_jce_trace_read_observed_internal
#define jce_trace_get_stats controlled_jce_trace_get_stats
#define jce_trace_capture_generation controlled_jce_trace_capture_generation
#define jce_trace_capture_valid_internal controlled_jce_trace_capture_valid_internal
#define jce_trace_capture_start controlled_jce_trace_capture_start
#define jce_trace_capture_start_mode controlled_jce_trace_capture_start_mode
#define jce_trace_capture_poll controlled_jce_trace_capture_poll
#define jce_trace_capture_quiesce controlled_jce_trace_capture_quiesce
#define jce_trace_capture_stop controlled_jce_trace_capture_stop
#define jce_trace_chrome_encode_batch_into controlled_jce_trace_chrome_encode_batch_into
#define jce_trace_chrome_encode_batch controlled_jce_trace_chrome_encode_batch
#define jce_trace_export_chrome_json controlled_jce_trace_export_chrome_json
#ifndef JCE_TEST_TRACE_PUBLICATION_OWNER
#define JCE_TEST_TRACE_PUBLICATION_OWNER "os/core/jce_trace.c"
#endif
#include JCE_TEST_TRACE_PUBLICATION_OWNER
#include "os/core/jce_trace_capture.c"
#undef SDL_SetAtomicInt

static int trace_enabled_store_port(SDL_AtomicInt *value, int desired)
{
    if (inject_enable_exhaustion && value == &s_enabled && desired == 1) {
        inject_enable_exhaustion = false;
        ++enable_exhaustion_hits;
        SDL_SetAtomicU32(&s_write_sequence, UINT32_MAX - 1u);
        uint32_t sequence;
        /* Actual producer reserve exhausts before the setter's native write. */
        (void)trace_sequence_reserve(&sequence);
    }
    return SDL_SetAtomicInt(value, desired);
}

void setUp(void)
{
    inject_enable_exhaustion = false;
    enable_exhaustion_hits = 0;
    jce_trace_reset();
    jce_trace_set_enabled(true);
}
void tearDown(void)
{
    inject_enable_exhaustion = false;
    s_thread.suppression = 0;
    jce_trace_task_restore_id(0);
    jce_trace_set_enabled(false);
}

static void test_reserved_slot_waits_until_publication(void)
{
    JceTraceCursor cursor = {1, 0};
    JceTraceEvent event;
    TraceSlot *slot = &s_slots[0];
    /* Deterministic pause at the actual producer reserve/publication seam. */
    uint32_t sequence = 0;
    TEST_ASSERT_TRUE(trace_sequence_reserve(&sequence));
    TEST_ASSERT_EQUAL_UINT32(1, sequence);
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read(&cursor, &event, 1));
    TEST_ASSERT_EQUAL_UINT(1, cursor.next_sequence);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    SDL_LockSpinlock(&slot->lock);
    SDL_zero(slot->event);
    slot->event.sequence = 1;
    slot->event.value_u64 = 42;
    SDL_SetAtomicU32(&slot->published, 1);
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read(&cursor, &event, 1));
    TEST_ASSERT_EQUAL_UINT(1, cursor.next_sequence);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    SDL_UnlockSpinlock(&slot->lock);
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&cursor, &event, 1));
    TEST_ASSERT_EQUAL_UINT(1, event.sequence);
    TEST_ASSERT_EQUAL_UINT64(42, event.value_u64);
    TEST_ASSERT_EQUAL_UINT(2, cursor.next_sequence);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
}

static void test_actual_overwrite_remains_loss(void)
{
    JceTraceCursor cursor = {1, 0};
    JceTraceEvent event;
    for (unsigned i = 0; i < TRACE_CAPACITY + 3; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "overwrite", i);
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&cursor, &event, 1));
    TEST_ASSERT_EQUAL_UINT(4, event.sequence);
    TEST_ASSERT_EQUAL_UINT64(3, cursor.lost_events);
    TEST_ASSERT_EQUAL_UINT(5, cursor.next_sequence);
}

static void test_delayed_old_publisher_cannot_replace_new_wrap(void)
{
    JceTraceEvent old_event;
    JceTraceEvent event;
    JceTraceCursor cursor = {1, 0};
    for (unsigned i = 0; i < TRACE_CAPACITY + 1; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "new-wrap", i);
    SDL_zero(old_event);
    old_event.sequence = 1;
    trace_publish_slot(&s_slots[0], &old_event);
    TEST_ASSERT_EQUAL_UINT(TRACE_CAPACITY + 1,
                          SDL_GetAtomicU32(&s_slots[0].published));
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&cursor, &event, 1));
    TEST_ASSERT_EQUAL_UINT64(1, cursor.lost_events);
    TEST_ASSERT_EQUAL_UINT(2, event.sequence);
    cursor.next_sequence = TRACE_CAPACITY + 1;
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&cursor, &event, 1));
    TEST_ASSERT_EQUAL_UINT(TRACE_CAPACITY + 1, event.sequence);
}

static void test_report_actual_slot_layout(void)
{
    /* Layout comparison only; this does not mirror the ring algorithm. */
    typedef struct LegacyTraceSlotLayout {
        SDL_AtomicInt published;
        JceTraceEvent event;
    } LegacyTraceSlotLayout;
    printf("trace slot layout: capacity=%u lock_bytes=%zu slot_bytes=%zu legacy_slot_bytes=%zu ring_bytes=%zu legacy_ring_bytes=%zu\n",
           TRACE_CAPACITY, sizeof(SDL_SpinLock), sizeof(TraceSlot),
           sizeof(LegacyTraceSlotLayout), sizeof(s_slots),
           sizeof(LegacyTraceSlotLayout) * TRACE_CAPACITY);
    TEST_ASSERT_TRUE(sizeof(TraceSlot) >= sizeof(JceTraceEvent));
}

static void test_unsigned_sequence_crosses_signed_boundary(void)
{
    SDL_SetAtomicU32(&s_write_sequence, (uint32_t)INT_MAX - 1u);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "signed-boundary-a", 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "signed-boundary-b", 2);
    JceTraceCursor cursor = {(uint32_t)INT_MAX, 0};
    JceTraceEvent events[2];
    TEST_ASSERT_EQUAL_UINT(2, jce_trace_read(&cursor, events, 2));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)INT_MAX, events[0].sequence);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)INT_MAX + 1u, events[1].sequence);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)INT_MAX + 2u, cursor.next_sequence);
}

static void test_sequence_exhaustion_is_permanent_and_reader_never_wraps(void)
{
    SDL_SetAtomicU32(&s_write_sequence, UINT32_MAX - 2u);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "last-legal-sequence", 1);
    JceTraceCursor cursor = {UINT32_MAX - 1u, 0};
    JceTraceEvent events[2];
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&cursor, events, 2));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1u, events[0].sequence);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, cursor.next_sequence);
    /* Two attempts expose the defined unsigned old-wrap mutation, without
     * signed overflow or asking any reader to make an unbounded pass. */
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "exhaust-sequence", 2);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "must-not-wrap", 3);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, SDL_GetAtomicU32(&s_write_sequence));
    TEST_ASSERT_FALSE(jce_trace_capture_valid_internal());
    TEST_ASSERT_FALSE(jce_trace_enabled());
    jce_trace_set_enabled(true);
    TEST_ASSERT_FALSE(jce_trace_enabled());
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read(&cursor, events, 2));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, cursor.next_sequence);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    jce_trace_reset();
    jce_trace_set_enabled(true);
    TEST_ASSERT_TRUE(jce_trace_capture_valid_internal());
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "after-explicit-reset", 4);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&cursor, events, 2));
    TEST_ASSERT_EQUAL_UINT32(1, events[0].sequence);
}

static void test_id_reservation_preserves_all_bits_and_refuses_full_space(void)
{
    trace_id_store_quiescent((uint64_t)INT_MAX - 1u);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)INT_MAX, jce_trace_next_id());
    TEST_ASSERT_EQUAL_UINT64((uint64_t)INT_MAX + 1u, jce_trace_next_id());
    trace_id_store_quiescent((uint64_t)UINT32_MAX - 1u);
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX, jce_trace_next_id());
    TEST_ASSERT_EQUAL_UINT64((uint64_t)UINT32_MAX + 1u, jce_trace_next_id());
    trace_id_store_quiescent(UINT64_MAX - 1u);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, jce_trace_next_id());
    uint64_t unchanged = 73;
    /* The public owner calls this exact reserve and aborts on refusal. */
    TEST_ASSERT_FALSE(trace_id_reserve(&unchanged));
    TEST_ASSERT_EQUAL_UINT64(73, unchanged);
    TEST_ASSERT_FALSE(trace_id_reserve(&unchanged));
}

typedef struct IdBatch { uint64_t ids[64]; } IdBatch;
static void reserve_id_batch(void *data)
{
    IdBatch *batch = data;
    for (unsigned i = 0; i < 64; ++i) batch->ids[i] = jce_trace_next_id();
}
static void test_concurrent_ids_cross_u32_without_duplicates(void)
{
    IdBatch batches[4];
    JceThread *workers[4];
    bool seen[256] = {false};
    const uint64_t base = (uint64_t)UINT32_MAX - 128u;
    trace_id_store_quiescent(base);
    unsigned created = 0;
    for (; created < 4; ++created) {
        workers[created] = jce_thread_create(reserve_id_batch, &batches[created], "trace-id-limit");
        if (!workers[created]) break;
    }
    for (unsigned i = 0; i < created; ++i) jce_thread_join(workers[i]);
    TEST_ASSERT_EQUAL_UINT(4, created);
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = 0; j < 64; ++j) {
        uint64_t id = batches[i].ids[j];
        TEST_ASSERT_TRUE(id > base && id <= base + 256u);
        unsigned index = (unsigned)(id - base - 1u);
        TEST_ASSERT_FALSE(seen[index]);
        seen[index] = true;
    }
}

typedef struct ThreadContextObservation {
    uint64_t initial_task, restored_task;
    bool initially_enabled, unbalanced_end, nested_enabled, final_enabled;
} ThreadContextObservation;
static void observe_thread_context(void *data)
{
    ThreadContextObservation *value = data;
    value->initial_task = jce_trace_task_current_id();
    value->initially_enabled = jce_trace_enabled();
    value->unbalanced_end = jce_trace_suppress_end();
    jce_trace_task_restore_id((UINT64_C(1) << 48) + 29u);
    value->restored_task = jce_trace_task_current_id();
    jce_trace_suppress_begin();
    jce_trace_suppress_begin();
    jce_trace_suppress_end();
    value->nested_enabled = jce_trace_enabled();
    jce_trace_suppress_end();
    value->final_enabled = jce_trace_enabled();
}
static void test_context_preserves_high_task_and_thread_local_nested_suppression(void)
{
    const uint64_t main_task = (UINT64_C(1) << 40) + 17u;
    jce_trace_task_restore_id(main_task);
    TEST_ASSERT_EQUAL_UINT64(main_task, jce_trace_task_current_id());
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    ThreadContextObservation observed = {0};
    JceThread *worker = jce_thread_create(observe_thread_context, &observed, "trace-context-limit");
    TEST_ASSERT_NOT_NULL(worker);
    jce_thread_join(worker);
    TEST_ASSERT_EQUAL_UINT64(0, observed.initial_task);
    TEST_ASSERT_TRUE(observed.initially_enabled);
    TEST_ASSERT_FALSE(observed.unbalanced_end);
    TEST_ASSERT_EQUAL_UINT64((UINT64_C(1) << 48) + 29u, observed.restored_task);
    TEST_ASSERT_FALSE(observed.nested_enabled);
    TEST_ASSERT_TRUE(observed.final_enabled);
    TEST_ASSERT_EQUAL_UINT64(main_task, jce_trace_task_current_id());
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_FALSE(jce_trace_enabled());
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_TRUE(jce_trace_enabled());
    TEST_ASSERT_FALSE(jce_trace_suppress_end());
    jce_trace_wait_begin(9, 0, "high-parent");
    JceTraceEvent event;
    JceTraceCursor cursor = {0};
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&cursor, &event, 1));
    TEST_ASSERT_EQUAL_UINT64(main_task, event.parent_id);
    jce_trace_wait_end(9, main_task, "high-parent", 1);
}

static void test_context_overflow_and_reset_registration_contract(void)
{
    s_thread.suppression = UINTPTR_MAX;
    TEST_ASSERT_FALSE(jce_trace_suppress_begin());
    TEST_ASSERT_EQUAL_UINT64(UINTPTR_MAX, s_thread.suppression);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_EQUAL_UINT64(UINTPTR_MAX - 1u, s_thread.suppression);
    s_thread.suppression = 0;
    jce_trace_thread_register("generation-a");
    jce_trace_thread_register("generation-a-again");
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.active_threads);
    jce_trace_task_restore_id(UINT64_C(0x100000017));
    jce_trace_reset();
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    jce_trace_thread_register("generation-b");
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(1, stats.active_threads);
    jce_trace_thread_unregister();
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_threads);
}

static void test_exhausted_ring_rejects_existing_and_new_consumers(void)
{
    const char *path = "trace-limit-publication.json";
    const char *resident = "trace-limit-resident.json";
    jce_fs_host_remove_file(path);
    jce_fs_host_remove_file(resident);
    JceTraceCapture *capture = jce_trace_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    char partial[1200];
    snprintf(partial, sizeof partial, "%s", capture->path);
    SDL_SetAtomicU32(&s_write_sequence, UINT32_MAX - 1u);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "terminal-attempt", 1);
    jce_trace_capture_poll(capture);
    JceTraceCaptureResult result;
    bool stopped = jce_trace_capture_stop(capture, &result);
    uint64_t size = 0;
    char *bytes = jce_fs_host_read_all(partial, &size);
    bool explicit_reason = bytes && strstr(bytes, "\"sequence_exhausted\":true");
    jce_fs_buffer_free(bytes);
    jce_fs_host_remove_file(partial);
    jce_fs_host_remove_file(path);
    TEST_ASSERT_FALSE(stopped);
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_FALSE(result.published);
    TEST_ASSERT_TRUE(result.sequence_exhausted);
    TEST_ASSERT_TRUE(explicit_reason);
    jce_trace_set_enabled(true);
    TEST_ASSERT_NULL(jce_trace_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(path));
    TEST_ASSERT_FALSE(jce_trace_export_chrome_json(resident));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(resident));
}

static void test_enable_store_rechecks_concurrent_exhaustion(void)
{
    jce_trace_set_enabled(false);
    inject_enable_exhaustion = true;
    jce_trace_set_enabled(true);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(1, enable_exhaustion_hits);
    TEST_ASSERT_FALSE(jce_trace_capture_valid_internal());
    TEST_ASSERT_FALSE(stats.enabled);
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&s_enabled));
    jce_trace_set_enabled(true);
    TEST_ASSERT_FALSE(jce_trace_enabled());
}

static void test_off_on_reregister_keeps_one_active_lifetime(void)
{
    jce_trace_thread_register("toggle-lifetime");
    const uint32_t generation = jce_trace_capture_generation();
    jce_trace_set_enabled(false);
    jce_trace_set_enabled(true);
    TEST_ASSERT_NOT_EQUAL(generation, jce_trace_capture_generation());
    jce_trace_thread_register("toggle-same-lifetime");
    JceTraceStats active, retired;
    jce_trace_get_stats(&active);
    jce_trace_thread_unregister();
    jce_trace_get_stats(&retired);
    TEST_ASSERT_EQUAL_UINT(1, active.active_threads);
    TEST_ASSERT_EQUAL_UINT(0, retired.active_threads);
    JceTraceEvent events[3];
    JceTraceCursor cursor = {0};
    TEST_ASSERT_EQUAL_UINT(2, jce_trace_read(&cursor, events, 3));
    TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_THREAD_REGISTER, events[0].type);
    TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_THREAD_UNREGISTER, events[1].type);
}

static void test_off_on_exit_without_reregister_retires_original_count(void)
{
    jce_trace_thread_register("toggle-direct-exit");
    jce_trace_set_enabled(false);
    jce_trace_set_enabled(true);
    jce_trace_thread_unregister();
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT(0, stats.active_threads);
    JceTraceEvent events[3];
    JceTraceCursor cursor = {0};
    TEST_ASSERT_EQUAL_UINT(2, jce_trace_read(&cursor, events, 3));
    TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_THREAD_UNREGISTER, events[1].type);
}

typedef struct RegistrationBarrier {
    SDL_Semaphore *ready, *retire;
} RegistrationBarrier;
static void retire_registration_after_reset(void *data)
{
    RegistrationBarrier *barrier = data;
    jce_trace_thread_register("old-accounting-epoch");
    SDL_SignalSemaphore(barrier->ready);
    SDL_WaitSemaphore(barrier->retire);
    jce_trace_thread_unregister();
}
static void test_reset_old_worker_exit_cannot_decrement_new_registration(void)
{
    RegistrationBarrier barrier = {SDL_CreateSemaphore(0), SDL_CreateSemaphore(0)};
    TEST_ASSERT_NOT_NULL(barrier.ready);
    TEST_ASSERT_NOT_NULL(barrier.retire);
    JceThread *worker = jce_thread_create(retire_registration_after_reset, &barrier, "trace-epoch-reset");
    TEST_ASSERT_NOT_NULL(worker);
    SDL_WaitSemaphore(barrier.ready);
    JceTraceStats before_reset, after_old_exit, after_new_exit;
    jce_trace_get_stats(&before_reset);
    /* Worker is quiescent at the barrier; reset explicitly starts accounting. */
    jce_trace_reset();
    jce_trace_thread_register("new-accounting-epoch");
    SDL_SignalSemaphore(barrier.retire);
    jce_thread_join(worker);
    jce_trace_get_stats(&after_old_exit);
    jce_trace_thread_unregister();
    jce_trace_get_stats(&after_new_exit);
    SDL_DestroySemaphore(barrier.ready);
    SDL_DestroySemaphore(barrier.retire);
    TEST_ASSERT_EQUAL_UINT(1, before_reset.active_threads);
    TEST_ASSERT_EQUAL_UINT(1, after_old_exit.active_threads);
    TEST_ASSERT_EQUAL_UINT(0, after_new_exit.active_threads);
}

static void test_observed_reader_distinguishes_pending_busy_limit_and_caught_up(void)
{
    JceTraceCursor observed = {0}, ordinary = {0};
    JceTraceEvent event, public_event;
    JceTraceReadObservation read;
    uint32_t reserved = 0;
    TEST_ASSERT_TRUE(trace_sequence_reserve(&reserved));
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read(&ordinary, &public_event, 1));
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read_observed_internal(&observed, &event, 1, &read));
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_READ_RESERVED_PENDING, read.stop);
    TEST_ASSERT_EQUAL_UINT(1, read.write_sequence); TEST_ASSERT_EQUAL_UINT(1, read.earliest_sequence);
    TEST_ASSERT_EQUAL_UINT(0, read.next_before); TEST_ASSERT_EQUAL_UINT(1, read.next_after);
    TEST_ASSERT_EQUAL_UINT(1, read.blocked_sequence); TEST_ASSERT_TRUE(read.published_valid);
    TEST_ASSERT_EQUAL_UINT(0, read.published_sequence); TEST_ASSERT_EQUAL_UINT64(0, read.lost_delta);
    TEST_ASSERT_EQUAL_UINT(ordinary.next_sequence, observed.next_sequence);
    TEST_ASSERT_EQUAL_UINT64(ordinary.lost_events, observed.lost_events);
    TraceSlot *slot = &s_slots[0]; SDL_LockSpinlock(&slot->lock);
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read(&ordinary, &public_event, 1));
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read_observed_internal(&observed, &event, 1, &read));
    SDL_zero(slot->event); slot->event.sequence = 1;
    SDL_SetAtomicU32(&slot->published, 1); SDL_UnlockSpinlock(&slot->lock);
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_READ_SLOT_BUSY, read.stop);
    TEST_ASSERT_FALSE(read.published_valid); TEST_ASSERT_EQUAL_UINT(0, read.published_sequence);
    TEST_ASSERT_EQUAL_UINT(1, read.blocked_sequence);
    TEST_ASSERT_EQUAL_UINT(ordinary.next_sequence, observed.next_sequence);
    TEST_ASSERT_EQUAL_UINT64(ordinary.lost_events, observed.lost_events);
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read(&ordinary, &public_event, 1));
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read_observed_internal(&observed, &event, 1, &read));
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_READ_CAUGHT_UP, read.stop);
    TEST_ASSERT_EQUAL_UINT(2, read.next_after); TEST_ASSERT_EQUAL_UINT(0, read.blocked_sequence);
    TEST_ASSERT_EQUAL_UINT(ordinary.next_sequence, observed.next_sequence);
    TEST_ASSERT_EQUAL_UINT64(ordinary.lost_events, observed.lost_events);
    TEST_ASSERT_EQUAL_UINT(public_event.sequence, event.sequence);
    TEST_ASSERT_EQUAL_UINT(public_event.type, event.type);
    TEST_ASSERT_EQUAL_UINT(public_event.category, event.category);
    TEST_ASSERT_EQUAL_UINT64(public_event.timestamp_ns, event.timestamp_ns);
    TEST_ASSERT_EQUAL_UINT64(public_event.thread_id, event.thread_id);
    TEST_ASSERT_EQUAL_UINT64(public_event.id, event.id);
    TEST_ASSERT_EQUAL_UINT64(public_event.parent_id, event.parent_id);
    TEST_ASSERT_EQUAL_UINT64(public_event.value_u64, event.value_u64);
    TEST_ASSERT_EQUAL_MEMORY(public_event.value_u32, event.value_u32, sizeof(event.value_u32));
    TEST_ASSERT_EQUAL_MEMORY(public_event.name, event.name, sizeof(event.name));
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "limited-a", 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "limited-b", 2);
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read_observed_internal(&observed, &event, 1, &read));
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_READ_BATCH_LIMIT, read.stop);
    TEST_ASSERT_EQUAL_UINT(3, read.write_sequence); TEST_ASSERT_EQUAL_UINT(3, read.next_after);
    TEST_ASSERT_EQUAL_UINT(0, read.blocked_sequence); TEST_ASSERT_FALSE(read.published_valid);
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read_observed_internal(&observed, NULL, 0, &read));
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_READ_INVALID_ARGS, read.stop);
    TEST_ASSERT_EQUAL_UINT(3, read.next_before); TEST_ASSERT_EQUAL_UINT(3, read.next_after);
    TEST_ASSERT_EQUAL_UINT(0, read.write_sequence); TEST_ASSERT_EQUAL_UINT(0, read.earliest_sequence);
}
static void test_observed_reader_keeps_actual_overwrite_accounting_and_empty_cursor(void)
{
    JceTraceCursor cursor = {1, 0}; JceTraceEvent event;
    JceTraceReadObservation read;
    TEST_ASSERT_EQUAL_UINT(0, jce_trace_read_observed_internal(&cursor, &event, 1, &read));
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_READ_CAUGHT_UP, read.stop);
    TEST_ASSERT_EQUAL_UINT(0, read.write_sequence); TEST_ASSERT_EQUAL_UINT(1, read.next_after);
    for (unsigned i = 0; i < TRACE_CAPACITY + 3; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "observed-overwrite", i);
    TEST_ASSERT_EQUAL_UINT(1, jce_trace_read_observed_internal(&cursor, &event, 1, &read));
    TEST_ASSERT_EQUAL_UINT(TRACE_CAPACITY + 3, read.write_sequence);
    TEST_ASSERT_EQUAL_UINT(4, read.earliest_sequence); TEST_ASSERT_EQUAL_UINT64(3, read.lost_delta);
    TEST_ASSERT_EQUAL_UINT64(3, cursor.lost_events); TEST_ASSERT_EQUAL_UINT(4, event.sequence);
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_READ_BATCH_LIMIT, read.stop);
}

int main(void)
{
    UNITY_BEGIN();
#if defined(JCE_TEST_TRACE_LIMIT_SEQUENCE_ONLY)
    RUN_TEST(test_sequence_exhaustion_is_permanent_and_reader_never_wraps);
#elif defined(JCE_TEST_TRACE_LIMIT_ID_ONLY)
    RUN_TEST(test_id_reservation_preserves_all_bits_and_refuses_full_space);
    RUN_TEST(test_concurrent_ids_cross_u32_without_duplicates);
#elif defined(JCE_TEST_TRACE_LIMIT_CONTEXT_ONLY)
    RUN_TEST(test_context_preserves_high_task_and_thread_local_nested_suppression);
#elif defined(JCE_TEST_TRACE_LIMIT_ENABLE_ONLY)
    RUN_TEST(test_enable_store_rechecks_concurrent_exhaustion);
#elif defined(JCE_TEST_TRACE_LIMIT_REGISTRATION_ONLY)
    RUN_TEST(test_off_on_reregister_keeps_one_active_lifetime);
    RUN_TEST(test_off_on_exit_without_reregister_retires_original_count);
    RUN_TEST(test_reset_old_worker_exit_cannot_decrement_new_registration);
#else
    RUN_TEST(test_observed_reader_distinguishes_pending_busy_limit_and_caught_up);
    RUN_TEST(test_observed_reader_keeps_actual_overwrite_accounting_and_empty_cursor);
    RUN_TEST(test_report_actual_slot_layout);
    RUN_TEST(test_reserved_slot_waits_until_publication);
    RUN_TEST(test_actual_overwrite_remains_loss);
    RUN_TEST(test_delayed_old_publisher_cannot_replace_new_wrap);
    RUN_TEST(test_unsigned_sequence_crosses_signed_boundary);
    RUN_TEST(test_sequence_exhaustion_is_permanent_and_reader_never_wraps);
    RUN_TEST(test_id_reservation_preserves_all_bits_and_refuses_full_space);
    RUN_TEST(test_concurrent_ids_cross_u32_without_duplicates);
    RUN_TEST(test_context_preserves_high_task_and_thread_local_nested_suppression);
    RUN_TEST(test_context_overflow_and_reset_registration_contract);
    RUN_TEST(test_exhausted_ring_rejects_existing_and_new_consumers);
    RUN_TEST(test_enable_store_rechecks_concurrent_exhaustion);
    RUN_TEST(test_off_on_reregister_keeps_one_active_lifetime);
    RUN_TEST(test_off_on_exit_without_reregister_retires_original_count);
    RUN_TEST(test_reset_old_worker_exit_cannot_decrement_new_registration);
#endif
    return UNITY_END();
}
