/* Exact production request observer + real core ring and OS atomics.
 * The counted port forwards real native allocations; it is not bgfx/GPU evidence.
 * Dedicated test threads emulate concurrent persistent allocator producers. */
#include "unity.h"
#include "renderer/jce_bgfx_allocator_trace_internal.h"
#include <string.h>
#include <jce/os/core/jce_allocator.h>

static JceBgfxAllocatorTrace observer;
static JceTraceEvent events[16384];
static uint32_t event_count;
static JceThread *workers[2];
enum { PORT_BLOCK_CAPACITY = JCE_BGFX_ALLOC_LEDGER_CAPACITY + 32 };
typedef struct PortBlock { void *ptr; size_t size; bool reserved; } PortBlock;
typedef struct CountedPort {
    JceAtomicI32 calls, gate_violations;
    JceMutex *mutex;
    bool fail, reset_capture, disable_capture, record_arguments;
    bool force_move, reuse_buffer, interleave_reuse, end_inflight, begin_inflight, snapshot_inflight;
    void *duplicate, *cached, *nested;
    size_t cached_size, size, alignment;
    PortBlock blocks[PORT_BLOCK_CAPACITY];
} CountedPort;
static CountedPort port;

static void *request(void *ptr, size_t size, size_t alignment);
/* Count a real allocator port. The optional retained block models an actual
 * pool reusing one backing address across distinct logical lifetimes; no
 * constant address is returned for multiple unrelated live allocations. */
static void *counted_reallocate(void *ptr, size_t size, size_t alignment, void *user)
{
    CountedPort *p = (CountedPort *)user;
    jce_atomic_i32_add(&p->calls, 1);
    /* Another producer may hold the gate legitimately. Single-threaded
     * probes alone can identify a gate inherited by this provider call. */
    if (p->record_arguments && jce_atomic_i32_load(&observer.metadata_gate) != 0)
        jce_atomic_i32_add(&p->gate_violations, 1);
    if (p->record_arguments) { p->size = size; p->alignment = alignment; }
    if (p->reset_capture) jce_trace_reset();
    if (p->disable_capture) jce_trace_set_enabled(false);
    if (p->snapshot_inflight) {
        p->snapshot_inflight = false;
        jce_bgfx_trace_summary(&observer, JCE_BGFX_ALLOC_SUMMARY_SNAPSHOT);
    }
    if (p->end_inflight) {
        p->end_inflight = false;
        jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    }
    if (p->begin_inflight) {
        p->begin_inflight = false;
        jce_bgfx_trace_begin(&observer, true, true);
    }
    if (p->duplicate) return p->duplicate; /* Deliberately broken allocator negative. */
    unsigned slot = PORT_BLOCK_CAPACITY;
    size_t old_size = 0;
    jce_mutex_lock(p->mutex);
    if (ptr) {
        for (unsigned i = 0; i < PORT_BLOCK_CAPACITY; ++i)
            if (!p->blocks[i].reserved && p->blocks[i].ptr == ptr) { slot = i; break; }
    }
    if (slot != PORT_BLOCK_CAPACITY) old_size = p->blocks[slot].size;
    else if (ptr || size) {
        for (unsigned i = 0; i < PORT_BLOCK_CAPACITY; ++i)
            if (!p->blocks[i].reserved && !p->blocks[i].ptr) { slot = i; break; }
    }
    if (slot != PORT_BLOCK_CAPACITY) p->blocks[slot].reserved = true;
    jce_mutex_unlock(p->mutex);
    void *result = NULL;
    if (!size) {
        if (p->reuse_buffer && ptr) { p->cached = ptr; p->cached_size = old_size; }
        else jce_free_raw(ptr);
    } else if (!p->fail) {
        if (!ptr && p->cached && size <= p->cached_size) {
            result = p->cached; p->cached = NULL; p->cached_size = 0;
        } else if (ptr && p->force_move) {
            result = jce_realloc_aligned(NULL, size, alignment);
            if (result) {
                memcpy(result, ptr, old_size < size ? old_size : size);
                if (p->interleave_reuse) {
                    /* The old address becomes available while the outer
                     * observer record remains RESERVED. Deterministic overlap. */
                    p->interleave_reuse = false; p->cached = ptr; p->cached_size = old_size;
                    p->nested = request(NULL, old_size, alignment);
                } else jce_free_raw(ptr);
            }
        } else result = jce_realloc_aligned(ptr, size, alignment);
    }
    jce_mutex_lock(p->mutex);
    if (slot != PORT_BLOCK_CAPACITY) {
        p->blocks[slot].ptr = size ? (result ? result : ptr) : NULL;
        p->blocks[slot].size = result ? size : old_size;
        p->blocks[slot].reserved = false;
    }
    jce_mutex_unlock(p->mutex);
    return result;
}
static void *request(void *ptr, size_t size, size_t alignment)
{
    return jce_bgfx_trace_reallocate(&observer, counted_reallocate, &port,
                                     ptr, size, alignment, 123);
}

static void drain(void)
{
    JceTraceCursor cursor;
    JceTraceEvent batch[64];
    uint32_t count;
    event_count = 0;
    jce_trace_cursor_init(&cursor);
    while ((count = jce_trace_read(&cursor, batch, 64)) != 0) {
        TEST_ASSERT_TRUE(event_count + count <= sizeof(events) / sizeof(events[0]));
        memcpy(events + event_count, batch, count * sizeof(*batch));
        event_count += count;
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
}

static unsigned count_named(const char *name)
{
    unsigned count = 0;
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER &&
            strcmp(events[i].name, name) == 0) ++count;
    return count;
}

static const JceTraceEvent *last_named(const char *name)
{
    for (uint32_t i = event_count; i > 0; --i)
        if (events[i - 1].type == JCE_TRACE_EVENT_COUNTER &&
            strcmp(events[i - 1].name, name) == 0) return &events[i - 1];
    TEST_FAIL_MESSAGE(name);
    return NULL;
}

static void expect_last(const char *name, uint64_t value)
{
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(value, last_named(name)->value_u64, name);
}

static void expect_request_value(uint64_t id, const char *name, uint64_t value)
{
    unsigned found = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (events[i].type != JCE_TRACE_EVENT_COUNTER || events[i].id != id ||
            strcmp(events[i].name, name) != 0) continue;
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(value, events[i].value_u64, name);
        ++found;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, found, name);
}

void setUp(void)
{
    memset(&observer, 0, sizeof(observer));
    memset(&port, 0, sizeof(port));
    memset(workers, 0, sizeof(workers));
    port.mutex = jce_mutex_create();
    TEST_ASSERT_NOT_NULL(port.mutex);
    port.record_arguments = true;
    event_count = 0;
    jce_trace_set_enabled(true);
    jce_trace_reset();
}

void tearDown(void)
{
    for (unsigned i = 0; i < 2; ++i)
        if (workers[i]) { jce_thread_join(workers[i]); workers[i] = NULL; }
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    jce_trace_set_enabled(false);
    for (unsigned i = 0; i < PORT_BLOCK_CAPACITY; ++i) {
        if (port.blocks[i].ptr) { jce_free_raw(port.blocks[i].ptr); port.blocks[i].ptr = NULL; }
    }
    if (port.cached) { jce_free_raw(port.cached); port.cached = NULL; }
    jce_mutex_destroy(port.mutex); port.mutex = NULL;
}

static void test_default_off_keeps_provider_and_records_nothing(void)
{
    jce_bgfx_trace_begin(&observer, false, true);
    void *buffer = request(NULL, 100, 16); TEST_ASSERT_NOT_NULL(buffer);
    buffer = request(buffer, 200, 32); TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NULL(request(buffer, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
    TEST_ASSERT_EQUAL_INT(3, jce_atomic_i32_load(&port.calls));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&observer.admitted));
    TEST_ASSERT_EQUAL_UINT64(0, observer.operations);
    TEST_ASSERT_EQUAL_UINT64(0, observer.backend_epoch);
}

static void test_trace_off_has_no_observation_even_when_opted_in(void)
{
    jce_trace_set_enabled(false);
    jce_bgfx_trace_begin(&observer, true, true);
    void *buffer = request(NULL, 100, 16); TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NULL(request(buffer, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
    TEST_ASSERT_EQUAL_INT(2, jce_atomic_i32_load(&port.calls));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&observer.admitted));
    TEST_ASSERT_EQUAL_UINT64(2, observer.finished);
    TEST_ASSERT_EQUAL_UINT64(0, observer.live_count);
    TEST_ASSERT_FALSE(observer.ledger_bad);
    TEST_ASSERT_TRUE(observer.events_bad);
}

static void test_allocate_reallocate_free_preserve_enclosing_parent(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    JceTraceScope owner = jce_trace_scope_begin("test.allocator.parent");
    void *buffer = request(NULL, 100, 16); TEST_ASSERT_NOT_NULL(buffer);
    buffer = request(buffer, 200, 32); TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NULL(request(buffer, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(owner.id, jce_trace_task_current_id());
    jce_trace_scope_end(owner, JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT64(owner.restore, jce_trace_task_current_id());
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    uint64_t ids[3] = {0, 0, 0};
    unsigned found = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (strcmp(events[i].name, "bgfx.alloc.request_index") != 0) continue;
        TEST_ASSERT_TRUE(found < 3);
        TEST_ASSERT_EQUAL_UINT64(found + 1, events[i].value_u64);
        TEST_ASSERT_EQUAL_UINT64(owner.id, events[i].parent_id);
        ids[found++] = events[i].id;
    }
    TEST_ASSERT_EQUAL_UINT32(3, found);
    TEST_ASSERT_NOT_EQUAL(ids[0], ids[1]);
    TEST_ASSERT_NOT_EQUAL(ids[1], ids[2]);
    for (unsigned i = 0; i < 3; ++i) {
        expect_request_value(ids[i], "bgfx.alloc.operation", i + 1);
        expect_request_value(ids[i], "bgfx.alloc.source_line", 123);
        expect_request_value(ids[i], "bgfx.alloc.operation_succeeded", 1);
        expect_request_value(ids[i], "bgfx.alloc.returned_nonnull", i != 2);
        for (uint32_t j = 0; j < event_count; ++j)
            if (events[j].id == ids[i])
                TEST_ASSERT_EQUAL_UINT64(owner.id, events[j].parent_id);
    }
    expect_request_value(ids[0], "bgfx.alloc.requested_size", 100);
    expect_request_value(ids[0], "bgfx.alloc.alignment", 16);
    expect_request_value(ids[0], "bgfx.alloc.input_nonnull", 0);
    expect_request_value(ids[1], "bgfx.alloc.requested_size", 200);
    expect_request_value(ids[1], "bgfx.alloc.alignment", 32);
    expect_request_value(ids[1], "bgfx.alloc.input_nonnull", 1);
    expect_request_value(ids[2], "bgfx.alloc.requested_size", 0);
    expect_last("bgfx.alloc.completed_requests", 3);
    expect_last("bgfx.alloc.failed_requests", 0);
    expect_last("bgfx.alloc.zero_size_frees", 1);
}

static void test_allocation_failure_is_not_a_successful_free(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    port.fail = true;
    TEST_ASSERT_NULL(request(NULL, 100, 16));
    void *foreign = jce_realloc_aligned(NULL, 32, 16); TEST_ASSERT_NOT_NULL(foreign);
    TEST_ASSERT_NULL(request(foreign, 200, 64));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_INIT_FAILED);
    drain();
    expect_last("bgfx.alloc.operation", JCE_BGFX_ALLOC_OP_REALLOC);
    expect_last("bgfx.alloc.input_nonnull", 1);
    expect_last("bgfx.alloc.operation_succeeded", 0);
    expect_last("bgfx.alloc.returned_nonnull", 0);
    expect_last("bgfx.alloc.failed_requests", 2);
    expect_last("bgfx.alloc.zero_size_frees", 0);
    expect_last("bgfx.alloc.summary_reason", JCE_BGFX_ALLOC_SUMMARY_INIT_FAILED);
    expect_last("bgfx.alloc.summary_quiescent", 1);
    TEST_ASSERT_EQUAL_INT(2, jce_atomic_i32_load(&port.calls));
}

static void test_zero_size_null_input_is_an_explicit_successful_free(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    TEST_ASSERT_NULL(request(NULL, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    expect_last("bgfx.alloc.operation", JCE_BGFX_ALLOC_OP_FREE);
    expect_last("bgfx.alloc.input_nonnull", 0);
    expect_last("bgfx.alloc.operation_succeeded", 1);
    expect_last("bgfx.alloc.failed_requests", 0);
    expect_last("bgfx.alloc.zero_size_frees", 1);
}

static void test_fixed_request_limit_keeps_all_provider_calls(void)
{
    /* The old 512-prefix control now verifies that the former cutoff cannot
     * hide later lifetimes. Distinct actual allocations retain forwarding. */
    jce_bgfx_trace_begin(&observer, true, true);
    const unsigned count = JCE_BGFX_ALLOC_TRACE_LIMIT + 7;
    for (unsigned i = 0; i < count; ++i) TEST_ASSERT_NOT_NULL(request(NULL, 64, 16));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    TEST_ASSERT_EQUAL_UINT32(count, count_named("bgfx.alloc.request_index"));
    TEST_ASSERT_EQUAL_UINT32(count, count_named("bgfx.alloc.operation_succeeded"));
    expect_last("bgfx.alloc.admitted_requests", count);
    expect_last("bgfx.alloc.completed_requests", count);
    expect_last("bgfx.alloc.suppressed_requests", 0);
    expect_last("bgfx.alloc.request_index", count);
    expect_last("bgfx.alloc.request_limit", 0);
    expect_last("bgfx.alloc.tracked_live_count", count);
    expect_last("bgfx.alloc.duplicate_addresses", 0);
    TEST_ASSERT_EQUAL_INT((int)count, jce_atomic_i32_load(&port.calls));
}

static void test_suppressed_count_saturates_without_signed_overflow(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    jce_atomic_i32_store(&observer.admitted, INT_MAX);
    jce_atomic_i32_store(&observer.suppressed, INT_MAX);
    for (unsigned i = 0; i < 3; ++i) request(NULL, 64, 16);
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    expect_last("bgfx.alloc.suppressed_requests", INT_MAX);
    expect_last("bgfx.alloc.counts_saturated", 1);
    TEST_ASSERT_EQUAL_UINT32(0, count_named("bgfx.alloc.request_index"));
    TEST_ASSERT_EQUAL_INT(3, jce_atomic_i32_load(&port.calls));
}

static void test_attempt_reset_is_quiescent_and_preserves_new_identity(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    const uint64_t old_id = observer.attempt_serial, old_epoch = observer.backend_epoch;
    void *buffer = request(NULL, 100, 16); TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NULL(request(buffer, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    jce_bgfx_trace_begin(&observer, true, true);
    TEST_ASSERT_NOT_EQUAL(old_id, observer.attempt_serial);
    TEST_ASSERT_EQUAL_UINT64(old_epoch + 1, observer.backend_epoch);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&observer.admitted));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&observer.completed));
    buffer = request(NULL, 200, 32); TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_NULL(request(buffer, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    expect_last("bgfx.alloc.admitted_requests", 2);
    expect_last("bgfx.alloc.completed_requests", 2);
    expect_last("bgfx.alloc.request_index", 2);
    expect_last("bgfx.alloc.attempt_serial", observer.attempt_serial);
    expect_last("bgfx.alloc.all_released", 1);
}

static void test_foreign_allocator_is_marked_unobserved(void)
{
    jce_bgfx_trace_begin(&observer, true, false);
    request(NULL, 100, 16);
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    expect_last("bgfx.alloc.bridge_active", 0);
    expect_last("bgfx.alloc.admitted_requests", 0);
    TEST_ASSERT_EQUAL_UINT32(0, count_named("bgfx.alloc.request_index"));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(&port.calls));
}

static void test_capture_restart_reports_gap_without_old_owner_match(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    const uint32_t old_generation = observer.origin_generation;
    port.reset_capture = true;
    request(NULL, 100, 16);
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    TEST_ASSERT_EQUAL_UINT32(0, count_named("bgfx.alloc.returned_nonnull"));
    TEST_ASSERT_EQUAL_UINT32(0, count_named("bgfx.alloc.operation_succeeded"));
    expect_last("bgfx.alloc.result_capture_gap", 1);
    expect_last("bgfx.alloc.origin_request_generation", old_generation);
    expect_last("bgfx.alloc.current_capture_generation", jce_trace_capture_generation());
    expect_last("bgfx.alloc.capture_gaps", 1);
    expect_last("bgfx.alloc.origin_capture_gap", 1);
    TEST_ASSERT_EQUAL_UINT64(0, last_named("bgfx.alloc.summary_reason")->id);
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(&port.calls));
}

static void test_capture_disabled_inside_provider_leaves_result_unmatched(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    port.disable_capture = true;
    request(NULL, 100, 16);
    jce_trace_set_enabled(true);
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    TEST_ASSERT_EQUAL_UINT32(1, count_named("bgfx.alloc.request_index"));
    TEST_ASSERT_EQUAL_UINT32(0, count_named("bgfx.alloc.operation_succeeded"));
    expect_last("bgfx.alloc.capture_gaps", 1);
    expect_last("bgfx.alloc.origin_capture_gap", 1);
    TEST_ASSERT_EQUAL_UINT64(0, last_named("bgfx.alloc.summary_reason")->id);
}

static void test_wide_request_values_are_clamped_only_in_observation(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    port.fail = true;
    request(NULL, SIZE_MAX, SIZE_MAX);
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    const bool wide = (uint64_t)SIZE_MAX > (uint64_t)INT64_MAX;
    expect_last("bgfx.alloc.requested_size", wide ? (uint64_t)INT64_MAX : (uint64_t)SIZE_MAX);
    expect_last("bgfx.alloc.alignment", wide ? (uint64_t)INT64_MAX : (uint64_t)SIZE_MAX);
    expect_last("bgfx.alloc.size_saturated", wide ? 1 : 0);
    expect_last("bgfx.alloc.alignment_saturated", wide ? 1 : 0);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)SIZE_MAX, (uint64_t)port.size);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)SIZE_MAX, (uint64_t)port.alignment);
    expect_last("bgfx.alloc.failed_requests", 1);
}

static void stream_requests(void *arg)
{
    const unsigned count = *(const unsigned *)arg;
    for (unsigned i = 0; i < count; ++i) request(NULL, 64, 16);
}

static void test_two_persistent_producers_share_one_atomic_request_limit(void)
{
    static const unsigned count = 320; /* Remains valid if a thread-create assertion aborts. */
    port.record_arguments = false; /* No mutable non-atomic fields in the port. */
    jce_bgfx_trace_begin(&observer, true, true);
    workers[0] = jce_thread_create(stream_requests, (void *)&count, "alloc-control-a");
    TEST_ASSERT_NOT_NULL(workers[0]);
    workers[1] = jce_thread_create(stream_requests, (void *)&count, "alloc-control-b");
    TEST_ASSERT_NOT_NULL(workers[1]);
    for (unsigned i = 0; i < 2; ++i) { jce_thread_join(workers[i]); workers[i] = NULL; }
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    drain();
    expect_last("bgfx.alloc.admitted_requests", 640);
    expect_last("bgfx.alloc.completed_requests", 640);
    expect_last("bgfx.alloc.suppressed_requests", 0);
    TEST_ASSERT_EQUAL_INT(640, jce_atomic_i32_load(&port.calls));
    TEST_ASSERT_EQUAL_UINT32(640, count_named("bgfx.alloc.request_index"));
    TEST_ASSERT_EQUAL_UINT32(640, count_named("bgfx.alloc.operation_succeeded"));
}

static void test_typed_cas_matches_expected_and_retains_inplace_storage(void)
{
    JceAtomicI32 value;
    memset(&value, 0, sizeof(value));
    TEST_ASSERT_TRUE(jce_atomic_i32_compare_exchange_internal(&value, 0, 1));
    TEST_ASSERT_FALSE(jce_atomic_i32_compare_exchange_internal(&value, 0, 2));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(&value));
    jce_atomic_i32_store(&value, INT_MAX);
    TEST_ASSERT_FALSE(jce_atomic_i32_compare_exchange_internal(&value, INT_MAX - 1, 0));
    TEST_ASSERT_TRUE(jce_atomic_i32_compare_exchange_internal(&value, INT_MAX, INT_MAX - 1));
    TEST_ASSERT_EQUAL_INT(INT_MAX - 1, jce_atomic_i32_load(&value));
    TEST_ASSERT_FALSE(jce_atomic_i32_compare_exchange_internal(NULL, 0, 1));
}


static uint64_t request_id_with_index(unsigned index)
{
    for (uint32_t i = 0; i < event_count; ++i)
        if (!strcmp(events[i].name, "bgfx.alloc.request_index") && events[i].value_u64 == index)
            return events[i].id;
    TEST_FAIL_MESSAGE("missing request index"); return 0;
}
static uint64_t request_value(uint64_t id, const char *name)
{
    for (uint32_t i = 0; i < event_count; ++i)
        if (events[i].id == id && !strcmp(events[i].name, name)) return events[i].value_u64;
    TEST_FAIL_MESSAGE(name); return 0;
}
static void test_real_moved_realloc_retains_identity_and_content(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    unsigned char *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    memset(a, 0x5a, 16); const uintptr_t old_address = (uintptr_t)a;
    port.force_move = true;
    unsigned char *b = request(a, 32, 16); TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_EQUAL(old_address, (uintptr_t)b);
    for (unsigned i = 0; i < 16; ++i) TEST_ASSERT_EQUAL_HEX8(0x5a, b[i]);
    TEST_ASSERT_NULL(request(b, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    const uint64_t birth = request_id_with_index(1), resize = request_id_with_index(2);
    TEST_ASSERT_EQUAL_UINT64(request_value(birth, "bgfx.alloc.allocation_id"),
                              request_value(resize, "bgfx.alloc.allocation_id"));
    expect_request_value(resize, "bgfx.alloc.old_size", 16);
    expect_request_value(resize, "bgfx.alloc.new_size", 32);
    expect_request_value(resize, "bgfx.alloc.allocation_revision", 1);
    expect_last("bgfx.alloc.tracked_live_bytes", 0);
    expect_last("bgfx.alloc.all_released", 1);
    expect_last("bgfx.alloc.event_history_complete", 1);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&port.gate_violations));
}
static void test_failed_resize_preserves_identity_bytes_and_later_free(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    port.fail = true; TEST_ASSERT_NULL(request(a, 32, 16));
    TEST_ASSERT_EQUAL_UINT64(1, observer.live_count);
    TEST_ASSERT_EQUAL_UINT64(16, observer.live_bytes);
    port.fail = false; TEST_ASSERT_NULL(request(a, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    const uint64_t birth = request_id_with_index(1), failed = request_id_with_index(2);
    TEST_ASSERT_EQUAL_UINT64(request_value(birth, "bgfx.alloc.allocation_id"),
                              request_value(failed, "bgfx.alloc.allocation_id"));
    expect_request_value(failed, "bgfx.alloc.operation_succeeded", 0);
    expect_request_value(failed, "bgfx.alloc.old_size", 16);
    expect_request_value(failed, "bgfx.alloc.new_size", 16);
    expect_request_value(failed, "bgfx.alloc.allocation_revision", 0);
    expect_last("bgfx.alloc.all_released", 1);
}
static void test_pool_address_reuse_gets_a_new_allocation_identity(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    port.reuse_buffer = true;
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NULL(request(a, 0, 0));
    void *b = request(NULL, 16, 16); TEST_ASSERT_EQUAL_PTR(a, b);
    TEST_ASSERT_NULL(request(b, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    const uint64_t first = request_id_with_index(1), second = request_id_with_index(3);
    TEST_ASSERT_NOT_EQUAL(request_value(first, "bgfx.alloc.allocation_id"),
                          request_value(second, "bgfx.alloc.allocation_id"));
    expect_last("bgfx.alloc.all_released", 1);
    expect_last("bgfx.alloc.unknown_inputs", 0);
}
static void test_reserved_old_address_reuse_before_resize_return_is_distinct(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    port.force_move = true; port.interleave_reuse = true;
    void *b = request(a, 32, 16); TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_PTR(a, port.nested);
    TEST_ASSERT_NOT_EQUAL((uintptr_t)b, (uintptr_t)port.nested);
    TEST_ASSERT_EQUAL_UINT64(2, observer.live_count);
    TEST_ASSERT_EQUAL_UINT64(48, observer.live_bytes);
    TEST_ASSERT_NULL(request(b, 0, 0));
    TEST_ASSERT_NULL(request(port.nested, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    const uint64_t birth = request_id_with_index(1), outer = request_id_with_index(2);
    const uint64_t nested = request_id_with_index(3);
    TEST_ASSERT_EQUAL_UINT64(request_value(birth, "bgfx.alloc.allocation_id"),
                              request_value(outer, "bgfx.alloc.allocation_id"));
    TEST_ASSERT_NOT_EQUAL(request_value(outer, "bgfx.alloc.allocation_id"),
                          request_value(nested, "bgfx.alloc.allocation_id"));
    expect_last("bgfx.alloc.all_released", 1);
    expect_last("bgfx.alloc.duplicate_addresses", 0);
    expect_last("bgfx.alloc.unknown_inputs", 0);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&port.gate_violations));
}
static void test_trace_pause_still_pairs_free_but_marks_history_incomplete(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    jce_trace_set_enabled(false);
    TEST_ASSERT_NULL(request(a, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(0, observer.live_count);
    jce_trace_set_enabled(true);
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.operations_total", 2);
    expect_last("bgfx.alloc.suppressed_requests", 1);
    expect_last("bgfx.alloc.ledger_complete", 1);
    expect_last("bgfx.alloc.event_history_complete", 0);
    expect_last("bgfx.alloc.all_released", 1);
}
static void test_full_ledger_preserves_real_provider_and_rejects_complete_witness(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    jce_trace_set_enabled(false); /* Exercise capacity without flooding the real ring. */
    for (unsigned i = 0; i < JCE_BGFX_ALLOC_LEDGER_CAPACITY + 1; ++i)
        TEST_ASSERT_NOT_NULL(request(NULL, 16, 16));
    TEST_ASSERT_EQUAL_UINT64(JCE_BGFX_ALLOC_LEDGER_CAPACITY, observer.live_count);
    TEST_ASSERT_EQUAL_UINT64(1, observer.overflow);
    jce_trace_set_enabled(true);
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.ledger_complete", 0);
    expect_last("bgfx.alloc.event_history_complete", 0);
    expect_last("bgfx.alloc.all_released", 0);
    TEST_ASSERT_EQUAL_INT(JCE_BGFX_ALLOC_LEDGER_CAPACITY + 1, jce_atomic_i32_load(&port.calls));
}
static void test_unknown_actual_input_is_not_fabricated_as_a_known_birth(void)
{
    void *a = jce_realloc_aligned(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    jce_bgfx_trace_begin(&observer, true, true);
    void *b = request(a, 32, 16); TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NULL(request(b, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    const uint64_t resize = request_id_with_index(1);
    expect_request_value(resize, "bgfx.alloc.input_known", 0);
    expect_request_value(resize, "bgfx.alloc.lineage_known", 0);
    expect_last("bgfx.alloc.unknown_inputs", 1);
    expect_last("bgfx.alloc.tracked_live_count", 0);
    expect_last("bgfx.alloc.ledger_complete", 0);
    expect_last("bgfx.alloc.all_released", 0);
}
static void test_duplicate_provider_address_is_an_explicit_incomplete_negative(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    port.duplicate = a; TEST_ASSERT_EQUAL_PTR(a, request(NULL, 16, 16));
    port.duplicate = NULL; TEST_ASSERT_NULL(request(a, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_request_value(request_id_with_index(2), "bgfx.alloc.paired", 0);
    expect_last("bgfx.alloc.duplicate_addresses", 1);
    expect_last("bgfx.alloc.ledger_complete", 0);
    expect_last("bgfx.alloc.all_released", 0);
}
static void test_identity_exhaustion_forwards_without_reusing_identity(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    observer.next_allocation = (uint64_t)INT64_MAX;
    TEST_ASSERT_NOT_NULL(request(NULL, 16, 16));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.allocation_id", 0);
    expect_last("bgfx.alloc.identity_overflow", 1);
    expect_last("bgfx.alloc.ledger_complete", 0);
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(&port.calls));
}
static void test_terminal_during_provider_is_refused_without_resetting_records(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    port.end_inflight = true;
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(observer.attempt_open);
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(&observer.enabled));
    TEST_ASSERT_EQUAL_UINT64(1, observer.end_refused);
    TEST_ASSERT_NULL(request(a, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.tracked_live_count", 0);
    expect_last("bgfx.alloc.ledger_complete", 0);
    expect_last("bgfx.alloc.all_released", 0);
    bool found_unquiet = false;
    for (unsigned i = 0; i < event_count; ++i)
        if (!strcmp(events[i].name, "bgfx.alloc.summary_quiescent") && !events[i].value_u64)
            found_unquiet = true;
    TEST_ASSERT_TRUE(found_unquiet);
}
static void test_begin_during_provider_is_refused_without_erasing_birth(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    const uint64_t epoch = observer.backend_epoch;
    port.begin_inflight = true;
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT64(epoch, observer.backend_epoch);
    TEST_ASSERT_EQUAL_UINT64(1, observer.operations);
    TEST_ASSERT_EQUAL_UINT64(1, observer.begin_refused);
    TEST_ASSERT_NULL(request(a, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.ledger_complete", 0);
    expect_last("bgfx.alloc.tracked_live_count", 0);
}
static void test_backend_epoch_survives_trace_identity_reset_between_sessions(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    const uint64_t old_epoch = observer.backend_epoch, old_capture_id = observer.attempt_serial;
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NULL(request(a, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    jce_trace_reset();
    jce_bgfx_trace_begin(&observer, true, true);
    TEST_ASSERT_EQUAL_UINT64(old_capture_id, observer.attempt_serial);
    TEST_ASSERT_EQUAL_UINT64(old_epoch + 1, observer.backend_epoch);
    a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_NULL(request(a, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.backend_epoch", old_epoch + 1);
    expect_last("bgfx.alloc.all_released", 1);
}
static void test_terminal_retains_known_live_bytes_without_claiming_release(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    TEST_ASSERT_NOT_NULL(request(NULL, 64, 16));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.tracked_live_count", 1);
    expect_last("bgfx.alloc.tracked_live_bytes", 64);
    expect_last("bgfx.alloc.ledger_complete", 1);
    expect_last("bgfx.alloc.event_history_complete", 1);
    expect_last("bgfx.alloc.all_released", 0);
}
static void stream_lifetimes(void *arg)
{
    const unsigned count = *(const unsigned *)arg;
    for (unsigned i = 0; i < count; ++i) {
        void *a = request(NULL, 16, 16);
        if (a) request(a, 0, 0);
    }
}
static void test_two_producers_release_actual_lifetimes_with_complete_ledger(void)
{
    static const unsigned count = 96;
    port.record_arguments = false;
    jce_bgfx_trace_begin(&observer, true, true);
    workers[0] = jce_thread_create(stream_lifetimes, (void *)&count, "allocation-life-a");
    workers[1] = jce_thread_create(stream_lifetimes, (void *)&count, "allocation-life-b");
    TEST_ASSERT_NOT_NULL(workers[0]); TEST_ASSERT_NOT_NULL(workers[1]);
    for (unsigned i = 0; i < 2; ++i) { jce_thread_join(workers[i]); workers[i] = NULL; }
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.operations_total", 384);
    expect_last("bgfx.alloc.operations_finished", 384);
    expect_last("bgfx.alloc.tracked_live_count", 0);
    expect_last("bgfx.alloc.tracked_live_bytes", 0);
    expect_last("bgfx.alloc.ledger_complete", 1);
    expect_last("bgfx.alloc.event_history_complete", 1);
    expect_last("bgfx.alloc.all_released", 1);
    expect_last("bgfx.alloc.duplicate_addresses", 0);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&port.gate_violations));
}
static void test_inflight_snapshot_is_not_a_terminal_or_provider_lock(void)
{
    jce_bgfx_trace_begin(&observer, true, true);
    port.snapshot_inflight = true;
    void *a = request(NULL, 16, 16); TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(observer.attempt_open);
    TEST_ASSERT_NULL(request(a, 0, 0));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    bool found_pending = false;
    for (unsigned i = 0; i < event_count; ++i)
        if (!strcmp(events[i].name, "bgfx.alloc.inflight_operations") && events[i].value_u64 == 1)
            found_pending = true;
    TEST_ASSERT_TRUE(found_pending);
    expect_last("bgfx.alloc.all_released", 1);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(&port.gate_violations));
}

static void test_backend_epoch_exhaustion_cannot_recycle_allocation_tuple(void)
{
    observer.backend_epoch = (uint64_t)INT64_MAX;
    jce_bgfx_trace_begin(&observer, true, true);
    TEST_ASSERT_NOT_NULL(request(NULL, 16, 16));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED); drain();
    expect_last("bgfx.alloc.backend_epoch", (uint64_t)INT64_MAX);
    expect_last("bgfx.alloc.allocation_id", 0);
    expect_last("bgfx.alloc.ledger_complete", 0);
    expect_last("bgfx.alloc.event_history_complete", 0);
    expect_last("bgfx.alloc.counts_saturated", 1);
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(&port.calls));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_off_keeps_provider_and_records_nothing);
    RUN_TEST(test_trace_off_has_no_observation_even_when_opted_in);
    RUN_TEST(test_allocate_reallocate_free_preserve_enclosing_parent);
    RUN_TEST(test_allocation_failure_is_not_a_successful_free);
    RUN_TEST(test_zero_size_null_input_is_an_explicit_successful_free);
    RUN_TEST(test_fixed_request_limit_keeps_all_provider_calls);
    RUN_TEST(test_suppressed_count_saturates_without_signed_overflow);
    RUN_TEST(test_attempt_reset_is_quiescent_and_preserves_new_identity);
    RUN_TEST(test_foreign_allocator_is_marked_unobserved);
    RUN_TEST(test_capture_restart_reports_gap_without_old_owner_match);
    RUN_TEST(test_capture_disabled_inside_provider_leaves_result_unmatched);
    RUN_TEST(test_wide_request_values_are_clamped_only_in_observation);
    RUN_TEST(test_two_persistent_producers_share_one_atomic_request_limit);
    RUN_TEST(test_typed_cas_matches_expected_and_retains_inplace_storage);
    RUN_TEST(test_real_moved_realloc_retains_identity_and_content);
    RUN_TEST(test_failed_resize_preserves_identity_bytes_and_later_free);
    RUN_TEST(test_pool_address_reuse_gets_a_new_allocation_identity);
    RUN_TEST(test_reserved_old_address_reuse_before_resize_return_is_distinct);
    RUN_TEST(test_trace_pause_still_pairs_free_but_marks_history_incomplete);
    RUN_TEST(test_full_ledger_preserves_real_provider_and_rejects_complete_witness);
    RUN_TEST(test_unknown_actual_input_is_not_fabricated_as_a_known_birth);
    RUN_TEST(test_duplicate_provider_address_is_an_explicit_incomplete_negative);
    RUN_TEST(test_identity_exhaustion_forwards_without_reusing_identity);
    RUN_TEST(test_terminal_during_provider_is_refused_without_resetting_records);
    RUN_TEST(test_begin_during_provider_is_refused_without_erasing_birth);
    RUN_TEST(test_backend_epoch_survives_trace_identity_reset_between_sessions);
    RUN_TEST(test_terminal_retains_known_live_bytes_without_claiming_release);
    RUN_TEST(test_two_producers_release_actual_lifetimes_with_complete_ledger);
    RUN_TEST(test_inflight_snapshot_is_not_a_terminal_or_provider_lock);
    RUN_TEST(test_backend_epoch_exhaustion_cannot_recycle_allocation_tuple);
    return UNITY_END();
}
