/* Actual provider-driven owner tests; no graphics device or upstream headers. */
#include "renderer/jce_instance_upload.h"
#include "os/core/jce_memory.h"
#include <unity.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_thread.h>
#include <string.h>

#define TEST_EPOCH 100u
#define TEST_MIB 1048576u
#define TEST_HANDLES 64u
#define TEST_STAGING (JCE_INSTANCE_STAGE_TRACE_LEASE_MAX + 2u)
#define TEST_TRANSIENT_FLOATS 1024u

typedef struct FakeHandle {
    uint32_t capacity, bytes, creates, destroys;
    uint16_t stride;
    bool live;
} FakeHandle;

typedef struct FakeStaging {
    void *data;
    uint32_t bytes;
} FakeStaging;

typedef struct FakeDeferred {
    void *data;
    JceInstanceStageLease *lease;
} FakeDeferred;

typedef struct FakeProvider {
    FakeHandle handles[TEST_HANDLES];
    FakeStaging staging[TEST_STAGING];
    FakeDeferred deferred[TEST_STAGING];
    uint32_t deferred_count, nonnull_leases, null_leases;
    uint32_t metadata_calls, metadata_allocated, metadata_freed, metadata_live;
    uint16_t reuse_handle;
    bool defer_release, metadata_fails, reuse_next;
    float transient_data[TEST_TRANSIENT_FLOATS];
    uint32_t available, query_calls, query_requested, transient_calls;
    uint32_t transient_requested, transient_num, transient_size;
    uint16_t query_stride, transient_stride, transient_handle;
    uint32_t create_calls, created, destroy_calls, live_handles, live_bytes;
    uint32_t staging_calls, staging_allocated, staging_freed, backend_freed;
    uint32_t staging_live, staging_bytes;
    uint32_t updates, update_offset, update_bytes;
    uint16_t update_handle;
    float update_first;
    uint32_t dynamic_binds, transient_binds, bind_offset, bind_count;
    uint16_t bind_handle;
    void *bind_encoder;
    JceInstanceTransient bound_transient;
    uint32_t warnings, warn_epoch, warn_requested, warn_actual;
    uint16_t warn_stride;
    bool transient_fails, transient_null, staging_fails, create_fails, update_fails;
} FakeProvider;

static FakeProvider fake;
static JceInstanceUploadOwner owner;
static JceTraceEvent *captured_events;

static uint32_t fake_available(void *user, uint32_t requested, uint16_t stride)
{
    FakeProvider *f = (FakeProvider *)user;
    ++f->query_calls;
    f->query_requested = requested;
    f->query_stride = stride;
    return f->available;
}

static bool fake_allocate_transient(void *user, uint32_t requested,
                                     uint16_t stride, JceInstanceTransient *out)
{
    FakeProvider *f = (FakeProvider *)user;
    ++f->transient_calls;
    f->transient_requested = requested;
    if (f->transient_fails) return false;
    memset(out, 0, sizeof *out);
    out->data = f->transient_null ? NULL : (uint8_t *)f->transient_data;
    out->num = f->transient_num == UINT32_MAX ? requested : f->transient_num;
    out->size = f->transient_size == UINT32_MAX ? requested * stride : f->transient_size;
    out->stride = f->transient_stride ? f->transient_stride : stride;
    out->handle = f->transient_handle;
    out->offset = 128u;
    return true;
}

static uint16_t fake_create_dynamic(void *user, uint32_t capacity, uint16_t stride)
{
    FakeProvider *f = (FakeProvider *)user;
    const uint64_t bytes = (uint64_t)capacity * stride;
    ++f->create_calls;
    TEST_ASSERT_TRUE(bytes <= JCE_INSTANCE_UPLOAD_BUDGET_BYTES);
    TEST_ASSERT_TRUE((uint64_t)f->live_bytes + bytes <= JCE_INSTANCE_UPLOAD_BUDGET_BYTES);
    if (f->create_fails) return JCE_INSTANCE_UPLOAD_INVALID;
    TEST_ASSERT_TRUE(f->created < TEST_HANDLES);
    const uint16_t handle = f->reuse_next ? f->reuse_handle : (uint16_t)f->created++;
    f->reuse_next = false;
    TEST_ASSERT_TRUE(handle < f->created && !f->handles[handle].live);
    ++f->handles[handle].creates;
    f->handles[handle].capacity = capacity;
    f->handles[handle].bytes = (uint32_t)bytes;
    f->handles[handle].stride = stride;
    f->handles[handle].live = true;
    ++f->live_handles;
    f->live_bytes += (uint32_t)bytes;
    return handle;
}

static void fake_destroy_dynamic(void *user, uint16_t handle)
{
    FakeProvider *f = (FakeProvider *)user;
    TEST_ASSERT_TRUE(handle < f->created);
    TEST_ASSERT_TRUE(f->handles[handle].live);
    TEST_ASSERT_TRUE(f->handles[handle].destroys < f->handles[handle].creates);
    f->handles[handle].live = false;
    ++f->handles[handle].destroys;
    ++f->destroy_calls;
    --f->live_handles;
    f->live_bytes -= f->handles[handle].bytes;
}

static void *fake_allocate_staging(void *user, uint32_t bytes)
{
    FakeProvider *f = (FakeProvider *)user;
    ++f->staging_calls;
    f->staging_bytes = bytes;
    TEST_ASSERT_TRUE(bytes > 0u && bytes <= JCE_INSTANCE_UPLOAD_BUDGET_BYTES);
    if (f->staging_fails) return NULL;
    for (uint32_t i = 0; i < TEST_STAGING; ++i) {
        if (!f->staging[i].data) {
            f->staging[i].data = JCE_MALLOC(bytes);
            TEST_ASSERT_NOT_NULL(f->staging[i].data);
            ((float *)f->staging[i].data)[0] = 0.0f;
            f->staging[i].bytes = bytes;
            ++f->staging_allocated;
            ++f->staging_live;
            return f->staging[i].data;
        }
    }
    TEST_FAIL_MESSAGE("More staging allocations than pending reservations");
    return NULL;
}

static FakeStaging *find_staging(FakeProvider *f, void *data)
{
    for (uint32_t i = 0; i < TEST_STAGING; ++i)
        if (f->staging[i].data == data) return &f->staging[i];
    TEST_FAIL_MESSAGE("Staging freed or consumed more than once");
    return NULL;
}

static void release_staging(FakeProvider *f, FakeStaging *stage)
{
    JCE_FREE(stage->data);
    stage->data = NULL;
    stage->bytes = 0;
    --f->staging_live;
}

static void fake_free_staging(void *user, void *data)
{
    FakeProvider *f = (FakeProvider *)user;
    FakeStaging *stage = find_staging(f, data);
    ++f->staging_freed;
    release_staging(f, stage);
}

static bool fake_update_dynamic(void *user, uint16_t handle, uint32_t offset,
                                 void *data, uint32_t bytes)
{
    FakeProvider *f = (FakeProvider *)user;
    FakeStaging *stage = find_staging(f, data);
    TEST_ASSERT_TRUE(handle < f->created && f->handles[handle].live);
    TEST_ASSERT_TRUE(bytes > 0u && bytes <= stage->bytes);
    TEST_ASSERT_EQUAL_UINT32(0, bytes % f->handles[handle].stride);
    TEST_ASSERT_TRUE((uint64_t)offset + bytes / f->handles[handle].stride <=
                     f->handles[handle].capacity);
    ++f->updates;
    f->update_handle = handle;
    f->update_offset = offset;
    f->update_bytes = bytes;
    f->update_first = ((float *)data)[0];
    if (f->update_fails) return false;
    /* A successful provider consumes the real JCE staging allocation. */
    ++f->backend_freed;
    release_staging(f, stage);
    return true;
}

static void fake_bind_dynamic(void *user, void *encoder, uint16_t handle,
                              uint32_t offset, uint32_t count)
{
    FakeProvider *f = (FakeProvider *)user;
    TEST_ASSERT_TRUE(handle < f->created && f->handles[handle].live);
    TEST_ASSERT_TRUE((uint64_t)offset + count <= f->handles[handle].capacity);
    ++f->dynamic_binds;
    f->bind_encoder = encoder;
    f->bind_handle = handle;
    f->bind_offset = offset;
    f->bind_count = count;
}

static void fake_bind_transient(void *user, void *encoder,
                                const JceInstanceTransient *transient, uint32_t count)
{
    FakeProvider *f = (FakeProvider *)user;
    TEST_ASSERT_NOT_NULL(transient->data);
    TEST_ASSERT_TRUE(count <= transient->num);
    TEST_ASSERT_TRUE((uint64_t)count * transient->stride <= transient->size);
    ++f->transient_binds;
    f->bind_encoder = encoder;
    f->bind_count = count;
    f->bound_transient = *transient;
}

static void fake_warn_partial(void *user, uint32_t epoch, uint32_t requested,
                              uint32_t actual, uint16_t stride)
{
    FakeProvider *f = (FakeProvider *)user;
    ++f->warnings;
    f->warn_epoch = epoch;
    f->warn_requested = requested;
    f->warn_actual = actual;
    f->warn_stride = stride;
}

static const JceInstanceUploadProvider provider = {
    fake_available, fake_allocate_transient, fake_create_dynamic,
    fake_destroy_dynamic, fake_allocate_staging, fake_free_staging,
    fake_update_dynamic, fake_bind_dynamic, fake_bind_transient, fake_warn_partial,
    NULL, NULL, NULL
};

static void *fake_allocate_trace_metadata(void *user, size_t bytes)
{
    FakeProvider *f = (FakeProvider *)user;
    ++f->metadata_calls;
    if (f->metadata_fails) return NULL;
    void *data = JCE_MALLOC(bytes);
    TEST_ASSERT_NOT_NULL(data);
    ++f->metadata_allocated;
    ++f->metadata_live;
    return data;
}

static void fake_free_trace_metadata(void *user, void *data)
{
    FakeProvider *f = (FakeProvider *)user;
    JCE_FREE(data);
    ++f->metadata_freed;
    --f->metadata_live;
}

static bool fake_update_dynamic_traced(void *user, uint16_t handle, uint32_t offset,
                                       void *data, uint32_t bytes, JceInstanceStageLease *lease)
{
    FakeProvider *f = (FakeProvider *)user;
    FakeStaging *stage = find_staging(f, data);
    TEST_ASSERT_TRUE(handle < f->created && f->handles[handle].live);
    TEST_ASSERT_TRUE(bytes > 0u && bytes <= stage->bytes);
    TEST_ASSERT_TRUE((uint64_t)offset + bytes / f->handles[handle].stride <=
                     f->handles[handle].capacity);
    ++f->updates;
    f->update_handle = handle; f->update_offset = offset; f->update_bytes = bytes;
    f->update_first = ((float *)data)[0];
    if (lease) ++f->nonnull_leases;
    else ++f->null_leases;
    if (f->update_fails) return false;
    jce_instance_upload_trace_transfer(lease);
    if (f->defer_release) {
        TEST_ASSERT_TRUE(f->deferred_count < TEST_STAGING);
        f->deferred[f->deferred_count].data = data;
        f->deferred[f->deferred_count++].lease = lease;
    } else {
        ++f->backend_freed;
        release_staging(f, stage);
        jce_instance_upload_trace_release(lease);
    }
    return true;
}

static void release_deferred(uint32_t index)
{
    FakeDeferred *deferred = &fake.deferred[index];
    TEST_ASSERT_NOT_NULL(deferred->data);
    ++fake.backend_freed;
    release_staging(&fake, find_staging(&fake, deferred->data));
    jce_instance_upload_trace_release(deferred->lease);
    deferred->data = NULL;
    deferred->lease = NULL;
}

static const JceInstanceUploadProvider traced_provider = {
    fake_available, fake_allocate_transient, fake_create_dynamic,
    fake_destroy_dynamic, fake_allocate_staging, fake_free_staging,
    fake_update_dynamic, fake_bind_dynamic, fake_bind_transient, fake_warn_partial,
    fake_update_dynamic_traced, fake_allocate_trace_metadata, fake_free_trace_metadata
};

static void use_traced_provider(bool enabled)
{
    jce_instance_upload_owner_destroy(&owner);
    jce_instance_upload_owner_init(&owner, &traced_provider, &fake, 0u, 12u);
    jce_trace_set_enabled(enabled);
    jce_trace_reset();
}

static JceInstanceUploadLane *lane_for(uint16_t stride)
{
    for (uint32_t i = 0; i < sizeof owner.lanes / sizeof owner.lanes[0]; ++i)
        if (owner.lanes[i].stride == stride) return &owner.lanes[i];
    TEST_FAIL_MESSAGE("Qualified stride has no owner lane");
    return NULL;
}

static JceInstanceUpload reserve_dynamic(uint32_t count, uint16_t stride)
{
    JceInstanceUpload upload;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, count, stride, 128u, &upload));
    TEST_ASSERT_TRUE(upload.dynamic);
    TEST_ASSERT_EQUAL_UINT32(count, upload.count);
    TEST_ASSERT_NOT_NULL(upload.data);
    upload.data[0] = 42.0f;
    return upload;
}

static void commit_all(JceInstanceUpload *upload)
{
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, upload, upload->count));
    TEST_ASSERT_TRUE(upload->committed);
    TEST_ASSERT_NULL(upload->data);
}

static const JceTraceEvent *trace_scope_event(const JceTraceEvent *events,
    uint32_t count, uint64_t scope, JceTraceEventType type)
{
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == type && events[i].id == scope) return &events[i];
    return NULL;
}

static uint64_t token_scope(const JceTraceEvent *events, uint32_t count,
    const char *operation, uint64_t serial, const char *field, uint64_t value)
{
    for (uint32_t i = 0; i < count; ++i) {
        const JceTraceEvent *event = &events[i];
        if (event->type != JCE_TRACE_EVENT_COUNTER || event->id != serial ||
            event->value_u64 != value || strcmp(event->name, field) != 0) continue;
        const JceTraceEvent *begin = trace_scope_event(events, count,
            event->parent_id, JCE_TRACE_EVENT_TASK_BEGIN);
        if (begin && strcmp(begin->name, operation) == 0) return begin->id;
    }
    TEST_FAIL_MESSAGE("Expected token counter in its operation scope");
    return 0;
}

static void assert_scoped_counter(const JceTraceEvent *events, uint32_t count,
    uint64_t scope, uint64_t serial, const char *name, uint64_t value)
{
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER && events[i].id == serial &&
            events[i].parent_id == scope && events[i].value_u64 == value &&
            strcmp(events[i].name, name) == 0) return;
    TEST_FAIL_MESSAGE(name);
}

static void assert_upload_identity(const JceTraceEvent *events, uint32_t count,
    uint64_t scope, const JceInstanceUpload *upload, uint32_t actual, void *encoder)
{
    const struct IdentityField { const char *name; uint64_t value; } fields[] = {
        {"instance.token_generation", upload->generation},
        {"instance.serial", upload->serial},
        {"instance.allocation_serial", upload->allocation_serial},
        {"instance.epoch", upload->epoch},
        {"instance.epoch_serial", upload->epoch_serial},
        {"instance.handle", upload->dynamic ? upload->handle : upload->transient.handle},
        {"instance.offset", upload->offset},
        {"instance.requested", upload->requested},
        {"instance.count", upload->count},
        {"instance.actual", actual},
        {"instance.capacity", upload->dynamic ? upload->capacity : upload->transient.num},
        {"instance.stride", upload->stride},
        {"instance.route_dynamic", upload->dynamic ? 1u : 0u},
        {"instance.fallback_reason", upload->fallback_reason},
        {"instance.transient_offset", upload->transient.offset},
        {"instance.transient_size", upload->transient.size},
        {"instance.transient_num", upload->transient.num},
        {"instance.transient_stride", upload->transient.stride},
        {"instance.encoder_opaque", (uint64_t)(uintptr_t)encoder}
    };
    for (uint32_t i = 0; i < sizeof fields / sizeof fields[0]; ++i)
        assert_scoped_counter(events, count, scope, upload->serial, fields[i].name, fields[i].value);
}

static JceTraceEvent *capture_trace(uint32_t *count)
{
    JceTraceStats stats;
    JceTraceCursor cursor;
    jce_trace_get_stats(&stats);
    const uint32_t capacity = stats.resident_events ? stats.resident_events : 1u;
    JCE_FREE(captured_events);
    captured_events = (JceTraceEvent *)JCE_MALLOC((size_t)capacity * sizeof *captured_events);
    TEST_ASSERT_NOT_NULL(captured_events);
    jce_trace_cursor_init(&cursor);
    *count = 0;
    while (*count < capacity) {
        uint32_t chunk = capacity - *count;
        if (chunk > 128u) chunk = 128u;
        const uint32_t got = jce_trace_read(&cursor, captured_events + *count, chunk);
        if (!got) break;
        *count += got;
    }
    JceTraceEvent extra;
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &extra, 1u));
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    return captured_events;
}

typedef struct StageTraceObserved {
    uint64_t held_bytes, held_leases, peak, unavailable_total, capacity, pool_full_total;
    bool instrumented;
    uint64_t transfers, releases;
    bool unavailable, pool_full;
} StageTraceObserved;

static void observe_stage_trace(JceTraceCursor *cursor, StageTraceObserved *observed)
{
    JceTraceEvent events[128];
    uint32_t count;
    while ((count = jce_trace_read(cursor, events, 128u)) != 0u) {
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (event->type != JCE_TRACE_EVENT_COUNTER) continue;
            if (strcmp(event->name, "instance.traced_backend_held_bytes") == 0)
                observed->held_bytes = event->value_u64;
            else if (strcmp(event->name, "instance.backend_held_lease_count") == 0)
                observed->held_leases = event->value_u64;
            else if (strcmp(event->name, "instance.staging_lease_peak") == 0)
                observed->peak = event->value_u64;
            else if (strcmp(event->name, "instance.staging_unavailable_total") == 0)
                observed->unavailable_total = event->value_u64;
            else if (strcmp(event->name, "instance.staging_slot_capacity") == 0)
                observed->capacity = event->value_u64;
            else if (strcmp(event->name, "instance.staging_pool_full_total") == 0)
                observed->pool_full_total = event->value_u64;
            else if (strcmp(event->name, "instance.staging_instrumented_scope") == 0)
                observed->instrumented = event->value_u64 == 1u;
            else if (strcmp(event->name, "instance.staging_transfer_bytes") == 0)
                observed->transfers += event->value_u64;
            else if (strcmp(event->name, "instance.staging_release_bytes") == 0)
                observed->releases += event->value_u64;
            else if (strcmp(event->name, "instance.staging_trace_unavailable") == 0 &&
                     event->value_u64 == 1u) observed->unavailable = true;
            else if (strcmp(event->name, "instance.staging_trace_reason") == 0 &&
                     event->value_u64 == 4u) observed->pool_full = true;
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor->lost_events);
}

void setUp(void)
{
    jce_trace_set_enabled(false);
    jce_thread_mark_main();
    memset(&fake, 0, sizeof fake);
    fake.transient_num = UINT32_MAX;
    fake.transient_size = UINT32_MAX;
    jce_instance_upload_owner_init(&owner, &provider, &fake, 0u, 11u);
    TEST_ASSERT_TRUE(owner.initialized);
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
    JCE_FREE(captured_events);
    captured_events = NULL;
    jce_instance_upload_owner_destroy(&owner);
    for (uint32_t i = 0; i < fake.deferred_count; ++i)
        if (fake.deferred[i].data) release_deferred(i);
    TEST_ASSERT_EQUAL_UINT32(0, fake.metadata_live);
    TEST_ASSERT_EQUAL_UINT32(fake.metadata_allocated, fake.metadata_freed);
    TEST_ASSERT_EQUAL_UINT32(0, fake.staging_live);
    TEST_ASSERT_EQUAL_UINT32(fake.staging_allocated,
                             fake.staging_freed + fake.backend_freed);
    TEST_ASSERT_EQUAL_UINT32(0, fake.live_handles);
    TEST_ASSERT_EQUAL_UINT32(0, fake.live_bytes);
    for (uint32_t i = 0; i < fake.created; ++i)
        TEST_ASSERT_EQUAL_UINT32(fake.handles[i].creates, fake.handles[i].destroys);
}

static void zero_handles_are_valid_and_candidates_publish_only_on_commit(void)
{
    JceInstanceUpload upload = reserve_dynamic(8u, 64u);
    TEST_ASSERT_EQUAL_UINT16(0, upload.handle);
    TEST_ASSERT_TRUE(upload.candidate);
    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(64u)->handle);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->capacity);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->cursor);
    TEST_ASSERT_TRUE(owner.pending_valid);
    TEST_ASSERT_EQUAL_UINT32(upload.capacity * 64u, owner.owned_bytes);
    commit_all(&upload);
    TEST_ASSERT_EQUAL_UINT16(0, lane_for(64u)->handle);
    TEST_ASSERT_EQUAL_UINT32(8, lane_for(64u)->cursor);
    TEST_ASSERT_EQUAL_FLOAT(42.0f, fake.update_first);
    TEST_ASSERT_EQUAL_UINT32(1, fake.backend_freed);
    TEST_ASSERT_EQUAL_UINT32(0, fake.staging_freed);
    TEST_ASSERT_TRUE(jce_instance_upload_bind(&owner, &upload, 8u));
    TEST_ASSERT_EQUAL_UINT16(0, fake.bind_handle);
    TEST_ASSERT_EQUAL_UINT32(1, fake.updates);
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &upload, 8u));
}

static void qualified_32_64_80_families_share_one_128_mib_budget(void)
{
    JceInstanceUpload first = reserve_dynamic(TEST_MIB, 64u);
    commit_all(&first);
    TEST_ASSERT_EQUAL_UINT32(64u * TEST_MIB, owner.owned_bytes);
    JceInstanceUpload second;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, TEST_MIB,
                                               80u, 128u, &second));
    TEST_ASSERT_EQUAL_UINT32(1, fake.created);
    TEST_ASSERT_EQUAL_UINT32(64u * TEST_MIB, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(80u)->handle);
    const uint32_t remaining = JCE_INSTANCE_UPLOAD_BUDGET_BYTES - owner.owned_bytes;
    second = reserve_dynamic(remaining / 80u, 80u);
    commit_all(&second);
    TEST_ASSERT_TRUE(owner.owned_bytes <= JCE_INSTANCE_UPLOAD_BUDGET_BYTES);
    TEST_ASSERT_EQUAL_UINT32(64, JCE_INSTANCE_UPLOAD_BUDGET_BYTES - owner.owned_bytes);
    JceInstanceUpload third = reserve_dynamic(2u, 32u);
    commit_all(&third);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_BUDGET_BYTES, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(owner.owned_bytes, fake.live_bytes);
    const uint16_t strides[] = {32u, 64u, 80u};
    for (uint32_t i = 0; i < 3u; ++i) {
        JceInstanceUpload rejected;
        TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u,
                                                   strides[i], 128u, &rejected));
        TEST_ASSERT_FALSE(owner.pending_valid);
    }
    TEST_ASSERT_EQUAL_UINT32(3, fake.created);
}

static void pending_candidates_and_retired_buffers_remain_inside_shared_budget(void)
{
    JceInstanceUpload first = reserve_dynamic(TEST_MIB / 2u, 64u);
    commit_all(&first);
    const uint32_t first_bytes = owner.owned_bytes;
    JceInstanceUpload growth = reserve_dynamic(first.capacity + 1u, 64u);
    TEST_ASSERT_TRUE(growth.candidate);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_BUDGET_BYTES, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(owner.owned_bytes, fake.live_bytes);
    TEST_ASSERT_EQUAL_UINT16(first.handle, lane_for(64u)->handle);
    TEST_ASSERT_EQUAL_UINT32(0, owner.retired_count);
    jce_instance_upload_discard(&owner, &growth);
    TEST_ASSERT_EQUAL_UINT32(first_bytes, owner.owned_bytes);

    growth = reserve_dynamic(first.capacity + 1u, 64u);
    commit_all(&growth);
    TEST_ASSERT_EQUAL_UINT32(1, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_BUDGET_BYTES, owner.owned_bytes);
    JceInstanceUpload rejected;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u, 80u, 128u, &rejected));
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u, 32u, 128u, &rejected));
    TEST_ASSERT_EQUAL_UINT32(3, fake.created);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_BUDGET_BYTES, owner.owned_bytes);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_BUDGET_BYTES - first_bytes, owner.owned_bytes);
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH + 1u, 4u, 80u, 128u, &rejected));
    rejected.data[0] = 1.0f;
    commit_all(&rejected);
}

static void full_retired_ring_blocks_replacement_but_allows_unused_family_creation(void)
{
    const uint16_t strides[] = {64u, 80u};
    for (uint32_t i = 0; i < 2u; ++i) {
        JceInstanceUpload upload = reserve_dynamic(1u, strides[i]);
        commit_all(&upload);
        for (uint32_t growth = 0; growth < 16u; ++growth) {
            const uint32_t capacity = lane_for(strides[i])->capacity;
            upload = reserve_dynamic(capacity, strides[i]);
            TEST_ASSERT_TRUE(upload.candidate);
            TEST_ASSERT_EQUAL_UINT32(capacity * 2u, upload.capacity);
            commit_all(&upload);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_RETIRED_MAX, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT32(34, fake.created);
    TEST_ASSERT_EQUAL_UINT32(0, fake.destroy_calls);
    TEST_ASSERT_TRUE(owner.owned_bytes < JCE_INSTANCE_UPLOAD_BUDGET_BYTES / 4u);
    const uint32_t owned = owner.owned_bytes;
    const uint32_t cursor = lane_for(64u)->cursor;
    const uint32_t staging_calls = fake.staging_calls;
    const uint16_t handle = lane_for(64u)->handle;
    JceInstanceUpload rejected;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH,
        lane_for(64u)->capacity, 64u, 128u, &rejected));
    TEST_ASSERT_FALSE(owner.pending_valid);
    TEST_ASSERT_EQUAL_UINT32(cursor, lane_for(64u)->cursor);
    TEST_ASSERT_EQUAL_UINT16(handle, lane_for(64u)->handle);
    TEST_ASSERT_EQUAL_UINT32(owned, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(staging_calls, fake.staging_calls);
    TEST_ASSERT_EQUAL_UINT32(34, fake.created);

    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(32u)->handle);
    JceInstanceUpload third = reserve_dynamic(1u, 32u);
    TEST_ASSERT_TRUE(third.candidate);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_RETIRED_MAX, owner.retired_count);
    commit_all(&third);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_UPLOAD_RETIRED_MAX, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT32(owned + 32u, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(35, fake.created);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &third, 1u, NULL));
}

static void geometric_growth_respects_each_callers_smaller_budget(void)
{
    const uint16_t strides[] = {32u, 64u, 80u};
    for (uint32_t i = 0; i < 3u; ++i) {
        JceInstanceUpload upload;
        TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 10000u,
                                                   strides[i], 1u, &upload));
        TEST_ASSERT_TRUE(upload.dynamic);
        TEST_ASSERT_TRUE(upload.capacity >= 10000u);
        TEST_ASSERT_TRUE((uint64_t)upload.capacity * strides[i] <= TEST_MIB);
        TEST_ASSERT_TRUE(upload.capacity <= TEST_MIB / strides[i]);
        upload.data[0] = 1.0f;
        commit_all(&upload);
    }
    TEST_ASSERT_TRUE(owner.owned_bytes <= 3u * TEST_MIB);
}

static void smaller_budget_preserves_current_slices_then_trims_at_epoch_edge(void)
{
    JceInstanceUpload first = reserve_dynamic(40000u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 4u));
    const uint32_t large_bytes = owner.owned_bytes;
    TEST_ASSERT_TRUE(large_bytes > TEST_MIB);
    JceInstanceUpload smaller;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u, 64u, 1u, &smaller));
    TEST_ASSERT_FALSE(smaller.candidate);
    TEST_ASSERT_EQUAL_UINT16(first.handle, smaller.handle);
    TEST_ASSERT_EQUAL_UINT32(4, smaller.offset);
    TEST_ASSERT_EQUAL_UINT32(large_bytes, owner.owned_bytes);
    smaller.data[0] = 1.0f;
    commit_all(&smaller);
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u, 64u, 0u, &smaller));
    TEST_ASSERT_EQUAL_UINT32(0, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(large_bytes, owner.owned_bytes);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &first, 4u, NULL));

    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->cursor);
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH + 1u,
                                               10000u, 64u, 1u, &smaller));
    TEST_ASSERT_TRUE(smaller.candidate);
    TEST_ASSERT_EQUAL_UINT32(1, fake.handles[first.handle].destroys);
    TEST_ASSERT_TRUE(owner.owned_bytes <= TEST_MIB);
    TEST_ASSERT_EQUAL_UINT32(0, owner.retired_count);
    smaller.data[0] = 1.0f;
    commit_all(&smaller);
    const uint16_t small_handle = smaller.handle;
    const uint32_t small_bytes = owner.owned_bytes;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH + 1u,
                                                1u, 64u, 0u, &smaller));
    TEST_ASSERT_EQUAL_UINT32(small_bytes, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, fake.handles[small_handle].destroys);
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH + 2u,
                                                1u, 64u, 0u, &smaller));
    TEST_ASSERT_EQUAL_UINT32(1, fake.handles[small_handle].destroys);
    TEST_ASSERT_EQUAL_UINT32(0, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->capacity);
    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(64u)->handle);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->cursor);
}

static void transient_admission_uses_actual_num_and_byte_size(void)
{
    JceInstanceUpload upload;
    fake.staging_fails = true;
    fake.available = 12u;
    fake.transient_num = 3u;
    fake.transient_size = 10u * 64u;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 12u, 64u, 128u, &upload));
    TEST_ASSERT_FALSE(upload.dynamic);
    TEST_ASSERT_EQUAL_UINT16(0, upload.transient.handle);
    TEST_ASSERT_EQUAL_UINT32(3, upload.count);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_calls);
    TEST_ASSERT_EQUAL_UINT32(12, fake.warn_requested);
    TEST_ASSERT_EQUAL_UINT32(3, fake.warn_actual);
    TEST_ASSERT_EQUAL_UINT16(64, fake.warn_stride);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 2u));
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &upload, 2u, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, fake.bind_count);
    TEST_ASSERT_EQUAL_UINT16(0, fake.bound_transient.handle);

    fake.transient_num = 8u;
    fake.transient_size = 2u * 80u + 79u;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 80u, 128u, &upload));
    TEST_ASSERT_EQUAL_UINT32(2, upload.count);
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &upload, 3u));
    TEST_ASSERT_FALSE(owner.pending_valid);
    TEST_ASSERT_EQUAL_UINT32(1, fake.transient_binds);
}

static void short_complete_fit_transient_retries_full_dynamic_and_restores_partial_after_create_failure(void)
{
    JceInstanceUpload upload;
    fake.available = 12u;
    fake.transient_num = 3u;
    fake.transient_size = 10u * 64u;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 12u, 64u, 128u, &upload));
    TEST_ASSERT_TRUE(upload.dynamic);
    TEST_ASSERT_TRUE(upload.candidate);
    TEST_ASSERT_EQUAL_UINT32(12, upload.count);
    TEST_ASSERT_EQUAL_UINT32(1, fake.transient_calls);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_calls);
    TEST_ASSERT_EQUAL_UINT32(12u * 64u, fake.staging_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, fake.warnings);
    upload.data[0] = 1.0f;
    commit_all(&upload);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &upload, 12u, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, fake.dynamic_binds);
    TEST_ASSERT_EQUAL_UINT32(0, fake.transient_binds);
    TEST_ASSERT_EQUAL_UINT32(12, lane_for(64u)->cursor);
    const uint32_t owned = owner.owned_bytes;

    fake.available = 8u;
    fake.transient_num = 4u;
    fake.transient_size = 8u * 80u;
    fake.create_fails = true;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 80u, 128u, &upload));
    TEST_ASSERT_FALSE(upload.dynamic);
    TEST_ASSERT_FALSE(upload.candidate);
    TEST_ASSERT_EQUAL_UINT32(4, upload.count);
    TEST_ASSERT_EQUAL_PTR(fake.transient_data, upload.data);
    TEST_ASSERT_EQUAL_UINT32(0, upload.capacity);
    TEST_ASSERT_EQUAL_UINT32(0, upload.offset);
    TEST_ASSERT_EQUAL_UINT32(2, fake.transient_calls);
    TEST_ASSERT_EQUAL_UINT32(2, fake.staging_allocated);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(owned, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(80u)->cursor);
    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(80u)->handle);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 3u));
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &upload, 3u, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, fake.transient_binds);
    TEST_ASSERT_EQUAL_UINT32(1, fake.updates);
    TEST_ASSERT_EQUAL_UINT32(0, fake.destroy_calls);
}

static void invalid_transient_allocation_uses_dynamic_fallback(void)
{
    JceInstanceUpload upload;
    fake.available = 4u;
    fake.transient_stride = 80u;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 4u, 64u, 128u, &upload));
    TEST_ASSERT_TRUE(upload.dynamic);
    TEST_ASSERT_EQUAL_UINT32(1, fake.transient_calls);
    commit_all(&upload);
    fake.transient_stride = 0;
    fake.transient_fails = true;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 4u, 32u, 128u, &upload));
    TEST_ASSERT_TRUE(upload.dynamic);
    commit_all(&upload);
    fake.transient_fails = false;
    fake.transient_size = 0;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 4u, 80u, 128u, &upload));
    TEST_ASSERT_TRUE(upload.dynamic);
    commit_all(&upload);
}

static void explicit_transient_reserve_keeps_headroom_for_later_draws(void)
{
    jce_instance_upload_owner_destroy(&owner);
    jce_instance_upload_owner_init(&owner, &provider, &fake, TEST_MIB, 12u);
    const uint32_t headroom = (TEST_MIB + 80u - 1u) / 80u;
    fake.available = headroom + 8u;
    JceInstanceUpload upload;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 80u, 128u, &upload));
    TEST_ASSERT_EQUAL_UINT32(headroom + 8u, fake.query_requested);
    TEST_ASSERT_EQUAL_UINT32(8, fake.transient_requested);
    TEST_ASSERT_FALSE(upload.dynamic);
    jce_instance_upload_discard(&owner, &upload);

    fake.available = headroom + 7u;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 80u, 128u, &upload));
    TEST_ASSERT_TRUE(upload.dynamic);
    TEST_ASSERT_EQUAL_UINT32(1, fake.transient_calls);
    commit_all(&upload);
}

static void partial_transient_is_last_fallback_and_never_consumes_reserved_headroom(void)
{
    jce_instance_upload_owner_destroy(&owner);
    jce_instance_upload_owner_init(&owner, &provider, &fake, 65u, 12u);
    JceInstanceUpload upload;
    fake.available = 2u;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 4u, 64u, 0u, &upload));
    TEST_ASSERT_EQUAL_UINT32(6, fake.query_requested);
    TEST_ASSERT_EQUAL_UINT32(0, fake.transient_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.staging_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.warn_actual);

    fake.available = 5u;
    fake.staging_fails = true;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 4u, 64u, 128u, &upload));
    TEST_ASSERT_FALSE(upload.dynamic);
    TEST_ASSERT_EQUAL_UINT32(3, upload.count);
    TEST_ASSERT_EQUAL_UINT32(3, fake.transient_requested);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.created);
    TEST_ASSERT_TRUE(jce_instance_upload_bind(&owner, &upload, 2u));
    TEST_ASSERT_EQUAL_UINT32(2, fake.bind_count);
}

static void one_pending_reservation_rejects_overlapping_begin(void)
{
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    const uint64_t serial = first.serial;
    JceInstanceUpload second;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 4u, 80u, 128u, &second));
    TEST_ASSERT_EQUAL_UINT32(1, fake.query_calls);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_calls);
    TEST_ASSERT_EQUAL_UINT32(1, fake.created);
    TEST_ASSERT_EQUAL_UINT64(serial, owner.pending.serial);
    jce_instance_upload_discard(&owner, &second);
    TEST_ASSERT_TRUE(owner.pending_valid);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    TEST_ASSERT_EQUAL_UINT32(2, lane_for(64u)->cursor);
    TEST_ASSERT_FALSE(owner.pending_valid);
}

static void actual_commit_counts_produce_disjoint_same_epoch_slices(void)
{
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    JceInstanceUpload second = reserve_dynamic(2u, 64u);
    TEST_ASSERT_FALSE(second.candidate);
    TEST_ASSERT_EQUAL_UINT16(first.handle, second.handle);
    TEST_ASSERT_EQUAL_UINT32(2, second.offset);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &second, 1u));
    TEST_ASSERT_EQUAL_UINT32(64, fake.update_bytes);
    TEST_ASSERT_EQUAL_UINT32(2, fake.update_offset);
    TEST_ASSERT_EQUAL_UINT32(3, lane_for(64u)->cursor);
    JceInstanceUpload third = reserve_dynamic(1u, 64u);
    TEST_ASSERT_EQUAL_UINT32(3, third.offset);
    commit_all(&third);
    TEST_ASSERT_TRUE(first.offset + first.actual <= second.offset);
    TEST_ASSERT_TRUE(second.offset + second.actual <= third.offset);
    TEST_ASSERT_EQUAL_UINT32(1, fake.created);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &first, 2u, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, fake.bind_offset);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &second, 1u, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, fake.bind_offset);
}

static void staging_allocation_failure_publishes_nothing_and_preserves_cursor(void)
{
    fake.staging_fails = true;
    JceInstanceUpload upload;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 64u, 128u, &upload));
    TEST_ASSERT_EQUAL_UINT32(0, fake.create_calls);
    TEST_ASSERT_EQUAL_UINT32(0, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(64u)->handle);
    TEST_ASSERT_FALSE(owner.pending_valid);
    fake.staging_fails = false;
    upload = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 2u));
    fake.staging_fails = true;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 2u, 64u, 128u, &upload));
    TEST_ASSERT_EQUAL_UINT32(2, lane_for(64u)->cursor);
    TEST_ASSERT_EQUAL_UINT16(0, lane_for(64u)->handle);
    TEST_ASSERT_EQUAL_UINT32(1, fake.create_calls);
}

static void create_failure_releases_staging_without_publishing_candidate(void)
{
    fake.create_fails = true;
    JceInstanceUpload upload;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 80u, 128u, &upload));
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_allocated);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(0, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(0, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(80u)->cursor);
    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(80u)->handle);
    TEST_ASSERT_FALSE(owner.pending_valid);
    fake.create_fails = false;
    upload = reserve_dynamic(8u, 80u);
    commit_all(&upload);
    TEST_ASSERT_EQUAL_UINT16(0, upload.handle);
}

static void failed_candidate_update_cancels_once_and_keeps_previous_lane(void)
{
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    const uint32_t bytes = owner.owned_bytes;
    JceInstanceUpload growth = reserve_dynamic(3u, 64u);
    TEST_ASSERT_TRUE(growth.candidate);
    const uint16_t candidate = growth.handle;
    fake.update_fails = true;
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &growth, 3u));
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(1, fake.handles[candidate].destroys);
    TEST_ASSERT_EQUAL_UINT32(bytes, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT16(first.handle, lane_for(64u)->handle);
    TEST_ASSERT_EQUAL_UINT32(2, lane_for(64u)->cursor);
    TEST_ASSERT_FALSE(owner.pending_valid);
    jce_instance_upload_discard(&owner, &growth);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &growth, 3u));
    fake.update_fails = false;
    growth = reserve_dynamic(2u, 64u);
    TEST_ASSERT_FALSE(growth.candidate);
    TEST_ASSERT_EQUAL_UINT32(2, growth.offset);
    commit_all(&growth);
}

static void failed_first_update_and_existing_lane_update_consume_no_cursor(void)
{
    JceInstanceUpload upload = reserve_dynamic(8u, 80u);
    fake.update_fails = true;
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &upload, 8u));
    TEST_ASSERT_EQUAL_UINT32(0, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT16(JCE_INSTANCE_UPLOAD_INVALID, lane_for(80u)->handle);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(80u)->cursor);
    TEST_ASSERT_EQUAL_UINT32(1, fake.destroy_calls);
    fake.update_fails = false;
    upload = reserve_dynamic(4u, 80u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 2u));
    const uint16_t published = upload.handle;
    const uint32_t destroys = fake.destroy_calls;
    upload = reserve_dynamic(2u, 80u);
    TEST_ASSERT_FALSE(upload.candidate);
    fake.update_fails = true;
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &upload, 2u));
    TEST_ASSERT_EQUAL_UINT16(published, lane_for(80u)->handle);
    TEST_ASSERT_EQUAL_UINT32(2, lane_for(80u)->cursor);
    TEST_ASSERT_EQUAL_UINT32(destroys, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(2, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(1, fake.backend_freed);
}

static void discard_aliases_and_zero_commit_release_each_resource_once(void)
{
    JceInstanceUpload upload = reserve_dynamic(4u, 32u);
    JceInstanceUpload alias = upload;
    jce_instance_upload_discard(&owner, &alias);
    jce_instance_upload_discard(&owner, &upload);
    jce_instance_upload_discard(&owner, &alias);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(1, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(0, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(32u)->cursor);
    upload = reserve_dynamic(4u, 32u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 0u));
    TEST_ASSERT_EQUAL_UINT32(2, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(2, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.updates);
    TEST_ASSERT_FALSE(owner.pending_valid);
    TEST_ASSERT_FALSE(upload.committed);
}

static void overcount_commit_discards_without_backend_update(void)
{
    JceInstanceUpload upload = reserve_dynamic(2u, 64u);
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &upload, 3u));
    TEST_ASSERT_EQUAL_UINT32(0, fake.updates);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(1, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(0, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->cursor);
}

static void epoch_advance_cancels_pending_invalidates_tokens_and_resets_cursors(void)
{
    JceInstanceUpload committed = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &committed, 2u));
    JceInstanceUpload pending = reserve_dynamic(2u, 64u);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH);
    TEST_ASSERT_TRUE(owner.pending_valid);
    TEST_ASSERT_EQUAL_UINT32(0, fake.staging_freed);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    TEST_ASSERT_FALSE(owner.pending_valid);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->cursor);
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &pending, 2u));
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &committed, 2u, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, fake.dynamic_binds);
    JceInstanceUpload current;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH + 1u, 4u, 64u, 128u, &current));
    TEST_ASSERT_FALSE(current.candidate);
    TEST_ASSERT_EQUAL_UINT32(0, current.offset);
    current.data[0] = 7.0f;
    commit_all(&current);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &current, 4u, NULL));
    const uint64_t generation = owner.generation;
    jce_instance_upload_owner_destroy(&owner);
    jce_instance_upload_owner_init(&owner, &provider, &fake, 0u, generation + 1u);
    TEST_ASSERT_TRUE(generation != owner.generation);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &current, 4u, NULL));
}

static void explicit_context_generation_rejects_zero_and_stale_reused_handles(void)
{
    JceInstanceUpload stale = reserve_dynamic(4u, 64u);
    commit_all(&stale);
    TEST_ASSERT_EQUAL_UINT16(0, stale.handle);
    jce_instance_upload_owner_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT32(1, fake.handles[0].destroys);
    TEST_ASSERT_EQUAL_UINT32(0, fake.staging_live);
    TEST_ASSERT_EQUAL_UINT32(fake.staging_allocated,
                             fake.staging_freed + fake.backend_freed);
    /* A fresh device can return the same numeric handle at the same owner
     * address and ordinal; only its explicit context generation differs. */
    memset(&fake, 0, sizeof fake);
    fake.transient_num = UINT32_MAX;
    fake.transient_size = UINT32_MAX;
    jce_instance_upload_owner_init(&owner, &provider, &fake, 0u, 0u);
    TEST_ASSERT_FALSE(owner.initialized);
    JceInstanceUpload current;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 4u, 64u, 128u, &current));
    TEST_ASSERT_EQUAL_UINT32(0, fake.query_calls);
    jce_instance_upload_owner_init(&owner, &provider, &fake, 0u, stale.generation + 1u);
    current = reserve_dynamic(4u, 64u);
    commit_all(&current);
    TEST_ASSERT_EQUAL_PTR(stale.owner, current.owner);
    TEST_ASSERT_EQUAL_UINT16(stale.handle, current.handle);
    TEST_ASSERT_EQUAL_UINT32(stale.epoch, current.epoch);
    TEST_ASSERT_TRUE(stale.generation != current.generation);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &stale, 4u, NULL));
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &stale, 4u));
    TEST_ASSERT_EQUAL_UINT32(0, fake.dynamic_binds);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &current, 4u, NULL));
}

static void uint32_submission_epoch_wrap_collects_once_and_invalidates_old_tokens(void)
{
    JceInstanceUpload first;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, 0u, 4u, 64u, 128u, &first));
    first.data[0] = 1.0f;
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    JceInstanceUpload growth;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, 0u, 3u, 64u, 128u, &growth));
    growth.data[0] = 1.0f;
    commit_all(&growth);
    TEST_ASSERT_EQUAL_UINT32(1, owner.retired_count);
    jce_instance_upload_owner_advance(&owner, 0u);
    TEST_ASSERT_EQUAL_UINT32(0, fake.destroy_calls);
    jce_instance_upload_owner_advance(&owner, UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, owner.epoch);
    TEST_ASSERT_EQUAL_UINT32(0, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT32(1, fake.handles[first.handle].destroys);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->cursor);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &first, 2u, NULL));
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &growth, 3u, NULL));
    JceInstanceUpload current;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, UINT32_MAX, 4u, 64u, 128u, &current));
    TEST_ASSERT_FALSE(current.candidate);
    TEST_ASSERT_EQUAL_UINT16(growth.handle, current.handle);
    TEST_ASSERT_EQUAL_UINT32(0, current.offset);
    current.data[0] = 1.0f;
    commit_all(&current);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &current, 4u, NULL));
    /* Raw ordinal 0 repeats, with the same live handle at the same owner
     * address; the monotonic epoch serial still rejects its ancient token. */
    jce_instance_upload_owner_advance(&owner, 0u);
    TEST_ASSERT_EQUAL_UINT32(growth.epoch, owner.epoch);
    TEST_ASSERT_TRUE(growth.epoch_serial != owner.epoch_serial);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &growth, 3u, NULL));
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, 0u, 4u, 64u, 128u, &current));
    TEST_ASSERT_EQUAL_UINT16(growth.handle, current.handle);
    current.data[0] = 1.0f;
    commit_all(&current);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &current, 4u, NULL));
    jce_instance_upload_owner_advance(&owner, 0u);
    TEST_ASSERT_EQUAL_UINT32(1, fake.destroy_calls);
}

static void growth_retains_old_slices_until_actual_submission_epoch_changes(void)
{
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    const uint32_t first_bytes = owner.owned_bytes;
    JceInstanceUpload growth = reserve_dynamic(first.capacity + 1u, 64u);
    TEST_ASSERT_TRUE(growth.candidate);
    TEST_ASSERT_EQUAL_UINT32(0, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(0, owner.retired_count);
    const uint32_t candidate_bytes = growth.capacity * growth.stride;
    commit_all(&growth);
    TEST_ASSERT_EQUAL_UINT32(1, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT32(first_bytes + candidate_bytes, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT16(first.handle, owner.retired[0].handle);
    TEST_ASSERT_EQUAL_UINT32(TEST_EPOCH, owner.retired[0].epoch);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &first, 2u, NULL));
    TEST_ASSERT_EQUAL_UINT16(first.handle, fake.bind_handle);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH);
    TEST_ASSERT_EQUAL_UINT32(0, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(1, owner.retired_count);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    TEST_ASSERT_EQUAL_UINT32(1, fake.handles[first.handle].destroys);
    TEST_ASSERT_EQUAL_UINT32(0, fake.handles[growth.handle].destroys);
    TEST_ASSERT_EQUAL_UINT32(0, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT32(candidate_bytes, owner.owned_bytes);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(64u)->cursor);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &first, 2u, NULL));
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 2u);
    TEST_ASSERT_EQUAL_UINT32(1, fake.destroy_calls);
}

static void committed_encoder_bind_is_read_only_and_rejects_invalid_counts(void)
{
    JceInstanceUpload upload = reserve_dynamic(4u, 80u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 3u));
    JceInstanceUploadOwner before;
    JceInstanceUpload token_before;
    memcpy(&before, &owner, sizeof before);
    memcpy(&token_before, &upload, sizeof token_before);
    int first_encoder = 1, second_encoder = 2;
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &upload, 3u, &first_encoder));
    TEST_ASSERT_EQUAL_PTR(&first_encoder, fake.bind_encoder);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &upload, 1u, &second_encoder));
    TEST_ASSERT_EQUAL_PTR(&second_encoder, fake.bind_encoder);
    TEST_ASSERT_EQUAL_UINT32(1, fake.updates);
    TEST_ASSERT_EQUAL_UINT32(2, fake.dynamic_binds);
    TEST_ASSERT_EQUAL_MEMORY(&before, &owner, sizeof before);
    TEST_ASSERT_EQUAL_MEMORY(&token_before, &upload, sizeof token_before);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &upload, 4u, &first_encoder));
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &upload, 0u, &first_encoder));
    JceInstanceUpload pending = reserve_dynamic(1u, 80u);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &pending, 1u, &first_encoder));
    TEST_ASSERT_EQUAL_UINT32(2, fake.dynamic_binds);
    jce_instance_upload_discard(&owner, &pending);
}

static void destroy_releases_active_retired_and_pending_resources_once(void)
{
    JceInstanceUpload a = reserve_dynamic(2u, 64u);
    commit_all(&a);
    a = reserve_dynamic(3u, 64u);
    commit_all(&a);
    JceInstanceUpload b = reserve_dynamic(2u, 80u);
    commit_all(&b);
    b = reserve_dynamic(3u, 80u);
    commit_all(&b);
    JceInstanceUpload pending = reserve_dynamic(3u, 32u);
    TEST_ASSERT_EQUAL_UINT32(2, owner.retired_count);
    TEST_ASSERT_EQUAL_UINT32(5, fake.live_handles);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_live);
    jce_instance_upload_owner_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT32(5, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(4, fake.backend_freed);
    TEST_ASSERT_FALSE(owner.initialized);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &a, 1u, NULL));
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &pending, 1u));
    jce_instance_upload_discard(&owner, &pending);
    jce_instance_upload_owner_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT32(5, fake.destroy_calls);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
}

static void overflowed_counts_unsupported_strides_and_zero_budgets_fail_without_allocation(void)
{
    const uint16_t qualified[] = {32u, 64u, 80u};
    const uint16_t unsupported[] = {0u, 1u, 16u, 48u, 79u, 96u, UINT16_MAX};
    JceInstanceUpload upload;
    for (uint32_t i = 0; i < 3u; ++i) {
        TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, UINT32_MAX,
                                                   qualified[i], 128u, &upload));
        TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH,
                              UINT32_MAX / qualified[i] + 1u, qualified[i], 128u, &upload));
    }
    for (uint32_t i = 0; i < sizeof unsupported / sizeof unsupported[0]; ++i)
        TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u,
                                                   unsupported[i], 128u, &upload));
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 0u, 64u, 128u, &upload));
    TEST_ASSERT_EQUAL_UINT32(0, fake.query_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.staging_calls);
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u, 64u, 0u, &upload));
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH,
                                               UINT32_MAX / 80u, 80u, UINT32_MAX, &upload));
    TEST_ASSERT_EQUAL_UINT32(0, fake.staging_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.create_calls);
    TEST_ASSERT_FALSE(owner.pending_valid);
}

static void opt_in_trace_reports_balanced_lifetime_scopes_counters_and_update_failure(void)
{
    JceTraceCursor cursor;
    JceTraceEvent disabled_event;
    JceTraceStats stats;
    jce_trace_reset();
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &disabled_event, 1u));

    jce_trace_set_enabled(true);
    jce_trace_reset();
    TEST_ASSERT_TRUE(jce_trace_enabled());
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &first, 2u, NULL));
    JceInstanceUpload growth = reserve_dynamic(3u, 64u);
    commit_all(&growth);
    JceInstanceUpload failed = reserve_dynamic(2u, 80u);
    fake.update_fails = true;
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &failed, 2u));
    fake.update_fails = false;
    JceInstanceUpload discarded = reserve_dynamic(1u, 32u);
    jce_instance_upload_discard(&owner, &discarded);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    jce_instance_upload_owner_destroy(&owner);

    jce_trace_cursor_init(&cursor);
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    TEST_ASSERT_TRUE(count > 0u);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    const char *scopes[] = {
        "renderer.instance.reserve", "renderer.instance.upload",
        "renderer.instance.bind", "renderer.instance.retire",
        "renderer.instance.discard", "renderer.instance.destroy"
    };
    uint32_t begins[6] = {0}, ends[6] = {0};
    uint32_t total_begins = 0, total_ends = 0;
    bool failed_upload = false, cancelled_discard = false;
    for (uint32_t i = 0; i < count; ++i) {
        const JceTraceEvent *event = &events[i];
        if (event->type != JCE_TRACE_EVENT_TASK_BEGIN &&
            event->type != JCE_TRACE_EVENT_TASK_END) continue;
        const bool begin = event->type == JCE_TRACE_EVENT_TASK_BEGIN;
        uint32_t matches = 0;
        TEST_ASSERT_TRUE(event->id != 0u);
        for (uint32_t j = 0; j < count; ++j) {
            if (events[j].id != event->id ||
                strcmp(events[j].name, event->name) != 0) continue;
            if ((begin && j > i && events[j].type == JCE_TRACE_EVENT_TASK_END) ||
                (!begin && j < i && events[j].type == JCE_TRACE_EVENT_TASK_BEGIN)) {
                TEST_ASSERT_EQUAL_UINT64(event->parent_id, events[j].parent_id);
                ++matches;
            }
        }
        TEST_ASSERT_EQUAL_UINT32(1, matches);
        if (begin) ++total_begins;
        else ++total_ends;
        for (uint32_t j = 0; j < 6u; ++j) {
            if (strcmp(event->name, scopes[j]) == 0) {
                if (begin) ++begins[j];
                else ++ends[j];
            }
        }
        if (!begin && strcmp(event->name, "renderer.instance.upload") == 0 &&
            event->value_u32[0] == JCE_TRACE_TASK_FAILED) failed_upload = true;
        if (!begin && strcmp(event->name, "renderer.instance.discard") == 0 &&
            event->value_u32[0] == JCE_TRACE_TASK_CANCELLED) cancelled_discard = true;
    }
    for (uint32_t i = 0; i < 6u; ++i) {
        TEST_ASSERT_TRUE(begins[i] > 0u);
        TEST_ASSERT_EQUAL_UINT32(begins[i], ends[i]);
    }
    TEST_ASSERT_TRUE(failed_upload);
    TEST_ASSERT_TRUE(cancelled_discard);
    const struct TraceCounterExpected {
        const char *name;
        uint64_t value;
    } counters[] = {
        {"instance.requested", 4u},
        {"instance.admitted", 2u},
        {"instance.stride", 64u},
        {"instance.logical_owned_bytes", 256u},
        {"instance.logical_live_bytes", 256u},
        {"instance.logical_retired_bytes", 256u},
        {"instance.logical_candidate_bytes", 512u},
        {"instance.pending_staging_bytes", 192u},
        {"instance.generation", 11u},
        {"instance.submission_epoch", TEST_EPOCH + 1u},
        {"instance.epoch_serial", 2u},
        {"instance.error_reason", JCE_INSTANCE_ERROR_UPDATE}
    };
    for (uint32_t i = 0; i < sizeof counters / sizeof counters[0]; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < count; ++j) {
            if (events[j].type == JCE_TRACE_EVENT_COUNTER &&
                events[j].category == JCE_TRACE_CATEGORY_RENDER &&
                strcmp(events[j].name, counters[i].name) == 0 &&
                events[j].value_u64 == counters[i].value) found = true;
        }
        TEST_ASSERT_TRUE_MESSAGE(found, counters[i].name);
    }
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(total_begins, total_ends);
    TEST_ASSERT_EQUAL_UINT64(total_begins, stats.work_items_started);
    TEST_ASSERT_EQUAL_UINT64(total_ends, stats.work_items_completed);
    jce_trace_set_enabled(false);
}

static void trace_off_uses_no_metadata_or_trace_lease(void)
{
    use_traced_provider(false);
    JceInstanceUpload upload = reserve_dynamic(4u, 64u);
    commit_all(&upload);
    TEST_ASSERT_EQUAL_UINT32(0, fake.metadata_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.nonnull_leases);
    TEST_ASSERT_EQUAL_UINT32(0, fake.null_leases);
    TEST_ASSERT_NULL(owner.stage_trace);
    jce_instance_upload_owner_destroy(&owner);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT32(1, fake.backend_freed);
}

static void trace_same_epoch_tokens_and_encoders_have_distinct_scoped_identities(void)
{
    use_traced_provider(true);
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    JceInstanceUpload second = reserve_dynamic(2u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &second, 1u));
    TEST_ASSERT_TRUE(first.serial != second.serial);
    TEST_ASSERT_EQUAL_UINT64(first.allocation_serial, second.allocation_serial);
    TEST_ASSERT_EQUAL_UINT16(first.handle, second.handle);
    int encoder_a = 1, encoder_b = 2;
    JceInstanceUploadOwner before;
    memcpy(&before, &owner, sizeof before);
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &first, 2u, &encoder_a));
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &first, 1u, &encoder_b));
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &second, 1u, &encoder_a));
    TEST_ASSERT_EQUAL_MEMORY(&before, &owner, sizeof before);
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t a = token_scope(events, count, "renderer.instance.bind", first.serial,
        "instance.encoder_opaque", (uint64_t)(uintptr_t)&encoder_a);
    const uint64_t b = token_scope(events, count, "renderer.instance.bind", first.serial,
        "instance.encoder_opaque", (uint64_t)(uintptr_t)&encoder_b);
    const uint64_t c = token_scope(events, count, "renderer.instance.bind", second.serial,
        "instance.encoder_opaque", (uint64_t)(uintptr_t)&encoder_a);
    TEST_ASSERT_TRUE(a != b && b != c && a != c);
    assert_upload_identity(events, count, a, &first, 2u, &encoder_a);
    assert_upload_identity(events, count, b, &first, 1u, &encoder_b);
    assert_upload_identity(events, count, c, &second, 1u, &encoder_a);
    assert_scoped_counter(events, count, a, first.serial, "instance.owner_observed", 1u);
    assert_scoped_counter(events, count, a, first.serial, "instance.owner_generation", owner.generation);
    bool generic = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].type != JCE_TRACE_EVENT_COUNTER ||
            strcmp(events[i].name, "instance.logical_owned_bytes") != 0) continue;
        generic = true;
        TEST_ASSERT_EQUAL_UINT64(events[i].parent_id, events[i].id);
        TEST_ASSERT_TRUE(events[i].parent_id != 0u);
        TEST_ASSERT_NOT_NULL(trace_scope_event(events, count, events[i].parent_id,
                                               JCE_TRACE_EVENT_TASK_BEGIN));
    }
    TEST_ASSERT_TRUE(generic);
}

static void trace_reused_handle_preserves_allocation_and_stale_owner_identity(void)
{
    use_traced_provider(true);
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    commit_all(&first);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    JceInstanceUpload trimmed;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH + 1u, 1u, 64u, 0u, &trimmed));
    fake.reuse_next = true; fake.reuse_handle = first.handle;
    JceInstanceUpload reused;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH + 1u, 4u, 64u, 128u, &reused));
    reused.data[0] = 1.0f;
    commit_all(&reused);
    TEST_ASSERT_EQUAL_UINT16(first.handle, reused.handle);
    TEST_ASSERT_TRUE(first.allocation_serial != reused.allocation_serial);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &first, 1u, NULL));
    jce_instance_upload_owner_destroy(&owner);
    fake.reuse_next = true; fake.reuse_handle = first.handle;
    jce_instance_upload_owner_init(&owner, &traced_provider, &fake, 0u, 13u);
    JceInstanceUpload current = reserve_dynamic(4u, 64u);
    commit_all(&current);
    TEST_ASSERT_EQUAL_UINT64(first.serial, current.serial);
    TEST_ASSERT_EQUAL_UINT16(first.handle, current.handle);
    TEST_ASSERT_EQUAL_UINT32(first.epoch, current.epoch);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &first, 1u, NULL));
    TEST_ASSERT_TRUE(jce_instance_upload_bind_encoder(&owner, &current, 4u, NULL));
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t stale = token_scope(events, count, "renderer.instance.bind",
        first.serial, "instance.owner_generation", 13u);
    assert_upload_identity(events, count, stale, &first, 0u, NULL);
    assert_scoped_counter(events, count, stale, first.serial, "instance.attempted_actual", 1u);
    assert_scoped_counter(events, count, stale, first.serial, "instance.token_generation", 12u);
    assert_scoped_counter(events, count, stale, first.serial, "instance.owner_observed", 1u);
    const uint64_t fresh = token_scope(events, count, "renderer.instance.bind",
        current.serial, "instance.token_generation", 13u);
    assert_upload_identity(events, count, fresh, &current, 4u, NULL);
    const uint64_t trim = token_scope(events, count, "renderer.instance.handle_destroy",
        first.serial, "instance.resource_reason", 2u);
    assert_scoped_counter(events, count, trim, first.serial,
                          "instance.allocation_serial", first.allocation_serial);
    TEST_ASSERT_EQUAL_UINT32(2, fake.handles[first.handle].destroys);
}

static void trace_rejected_partial_token_keeps_requested_count_and_current_epoch_separate(void)
{
    use_traced_provider(true);
    fake.available = 8u; fake.transient_num = 2u;
    fake.transient_size = 8u * 64u; fake.staging_fails = true;
    JceInstanceUpload partial;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 64u, 128u, &partial));
    TEST_ASSERT_FALSE(partial.dynamic);
    TEST_ASSERT_EQUAL_UINT32(8, partial.requested);
    TEST_ASSERT_EQUAL_UINT32(2, partial.count);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &partial, 1u));
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    TEST_ASSERT_FALSE(jce_instance_upload_bind_encoder(&owner, &partial, 1u, NULL));
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t stale = token_scope(events, count, "renderer.instance.bind",
        partial.serial, "instance.owner_epoch", TEST_EPOCH + 1u);
    assert_upload_identity(events, count, stale, &partial, 0u, NULL);
    assert_scoped_counter(events, count, stale, partial.serial, "instance.requested", 8u);
    assert_scoped_counter(events, count, stale, partial.serial, "instance.count", 2u);
    assert_scoped_counter(events, count, stale, partial.serial, "instance.epoch", TEST_EPOCH);
    assert_scoped_counter(events, count, stale, partial.serial,
                          "instance.owner_epoch_serial", owner.epoch_serial);
    assert_scoped_counter(events, count, stale, partial.serial, "instance.attempted_actual", 1u);
}

static void trace_candidate_cancel_retire_and_shutdown_keep_original_resource_identity(void)
{
    use_traced_provider(true);
    fake.create_fails = true;
    const uint64_t failed_serial = owner.serial + 1u;
    JceInstanceUpload failed;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 3u, 32u, 128u, &failed));
    TEST_ASSERT_EQUAL_UINT64(0, failed.serial);
    TEST_ASSERT_NULL(failed.data);
    fake.create_fails = false;
    JceInstanceUpload first = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &first, 2u));
    JceInstanceUpload cancelled = reserve_dynamic(3u, 64u);
    const JceInstanceUpload cancel_identity = cancelled;
    jce_instance_upload_discard(&owner, &cancelled);
    JceInstanceUpload growth = reserve_dynamic(3u, 64u);
    commit_all(&growth);
    JceInstanceUpload pending = reserve_dynamic(2u, 80u);
    const JceInstanceUpload pending_identity = pending;
    JceInstanceUpload blocked;
    TEST_ASSERT_FALSE(jce_instance_upload_begin(&owner, TEST_EPOCH, 1u, 32u, 128u, &blocked));
    TEST_ASSERT_EQUAL_UINT64(pending.serial, owner.pending.serial);
    jce_instance_upload_owner_advance(&owner, TEST_EPOCH + 1u);
    jce_instance_upload_owner_destroy(&owner);
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t failed_create = token_scope(events, count, "renderer.instance.candidate_create",
        failed_serial, "instance.resource_reason", JCE_INSTANCE_ERROR_CREATE);
    const JceTraceEvent *failed_end = trace_scope_event(events, count, failed_create,
        JCE_TRACE_EVENT_TASK_END);
    TEST_ASSERT_NOT_NULL(failed_end);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, failed_end->value_u32[0]);
    assert_scoped_counter(events, count, failed_create, failed_serial, "instance.requested", 3u);
    const uint64_t failed_free = token_scope(events, count, "renderer.instance.staging_cpu_free",
        failed_serial, "instance.staging_free_bytes", 96u);
    assert_scoped_counter(events, count, failed_free, failed_serial, "instance.token_generation", 12u);
    const uint64_t conflict = token_scope(events, count, "renderer.instance.reserve",
        0u, "instance.blocking_pending_serial", pending_identity.serial);
    assert_scoped_counter(events, count, conflict, 0u, "instance.requested", 1u);
    assert_scoped_counter(events, count, conflict, 0u, "instance.stride", 32u);
    const uint64_t create = token_scope(events, count, "renderer.instance.candidate_create",
        cancel_identity.serial, "instance.handle", cancel_identity.handle);
    assert_upload_identity(events, count, create, &cancel_identity, 0u, NULL);
    const uint64_t discard = token_scope(events, count, "renderer.instance.handle_destroy",
        cancel_identity.serial, "instance.resource_reason", 4u);
    assert_upload_identity(events, count, discard, &cancel_identity, 0u, NULL);
    const uint64_t retire = token_scope(events, count, "renderer.instance.retire_enqueue",
        first.serial, "instance.allocation_serial", first.allocation_serial);
    assert_upload_identity(events, count, retire, &first, first.actual, NULL);
    const uint64_t implicit = token_scope(events, count, "renderer.instance.implicit_cancel",
        pending_identity.serial, "instance.serial", pending_identity.serial);
    assert_upload_identity(events, count, implicit, &pending_identity, 0u, NULL);
    const uint64_t advanced = token_scope(events, count, "renderer.instance.handle_destroy",
        pending_identity.serial, "instance.resource_reason", 1u);
    assert_upload_identity(events, count, advanced, &pending_identity, 0u, NULL);
    const uint64_t shutdown = token_scope(events, count, "renderer.instance.handle_destroy",
        growth.serial, "instance.resource_reason", 3u);
    assert_upload_identity(events, count, shutdown, &growth, growth.actual, NULL);
}

static void trace_dynamic_recovery_retains_transient_discrepancy_reason(void)
{
    use_traced_provider(true);
    fake.available = 8u; fake.transient_num = 2u; fake.transient_size = 8u * 64u;
    JceInstanceUpload upload;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(&owner, TEST_EPOCH, 8u, 64u, 128u, &upload));
    TEST_ASSERT_TRUE(upload.dynamic);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_ERROR_TRANSIENT, upload.fallback_reason);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_ERROR_NONE, owner.last_error);
    upload.data[0] = 1.0f;
    commit_all(&upload);
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t reserve = token_scope(events, count, "renderer.instance.reserve",
        upload.serial, "instance.fallback_reason", JCE_INSTANCE_ERROR_TRANSIENT);
    assert_upload_identity(events, count, reserve, &upload, 0u, NULL);
    const uint64_t transfer = token_scope(events, count, "renderer.instance.staging_transfer",
        upload.serial, "instance.fallback_reason", JCE_INSTANCE_ERROR_TRANSIENT);
    assert_upload_identity(events, count, transfer, &upload, upload.actual, NULL);
}

static void trace_metadata_oom_keeps_upload_success_and_reports_capture_gap(void)
{
    use_traced_provider(true);
    fake.metadata_fails = true;
    JceInstanceUpload upload = reserve_dynamic(4u, 80u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 2u));
    TEST_ASSERT_EQUAL_UINT32(2, lane_for(80u)->cursor);
    TEST_ASSERT_FALSE(owner.pending_valid);
    TEST_ASSERT_NULL(owner.stage_trace);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_calls);
    TEST_ASSERT_EQUAL_UINT32(0, fake.metadata_allocated);
    TEST_ASSERT_EQUAL_UINT32(1, fake.null_leases);
    TEST_ASSERT_EQUAL_UINT32(1, fake.backend_freed);
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t failed = token_scope(events, count, "renderer.instance.upload",
        upload.serial, "instance.staging_trace_unavailable", 1u);
    assert_scoped_counter(events, count, failed, upload.serial, "instance.staging_trace_reason", 1u);
    fake.metadata_fails = false;
    JceInstanceUpload recovered = reserve_dynamic(2u, 80u);
    commit_all(&recovered);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_allocated);
    TEST_ASSERT_EQUAL_UINT32(1, fake.nonnull_leases);
    TEST_ASSERT_EQUAL_UINT32(4, lane_for(80u)->cursor);
    jce_instance_upload_owner_destroy(&owner);
    events = capture_trace(&count);
    const uint64_t summary = token_scope(events, count, "renderer.instance.staging_summary",
        0u, "instance.owner_generation", 12u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_unavailable_total", 1u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_state_oom_total", 1u);
    assert_scoped_counter(events, count, summary, 0u, "instance.traced_backend_held_bytes", 0u);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_freed);
}

static void trace_lease_exhaustion_preserves_rendering_and_streams_incomplete_footer(void)
{
    use_traced_provider(true);
    fake.defer_release = true;
    JceTraceCursor cursor;
    StageTraceObserved observed;
    memset(&observed, 0, sizeof observed);
    jce_trace_cursor_init(&cursor);
    for (uint32_t i = 0; i <= JCE_INSTANCE_STAGE_TRACE_LEASE_MAX; ++i) {
        JceInstanceUpload upload = reserve_dynamic(1u, 32u);
        commit_all(&upload);
        observe_stage_trace(&cursor, &observed);
    }
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_STAGE_TRACE_LEASE_MAX, fake.nonnull_leases);
    TEST_ASSERT_EQUAL_UINT32(1, fake.null_leases);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_STAGE_TRACE_LEASE_MAX + 1u, lane_for(32u)->cursor);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_allocated);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)JCE_INSTANCE_STAGE_TRACE_LEASE_MAX * 32u, observed.held_bytes);
    TEST_ASSERT_EQUAL_UINT64(JCE_INSTANCE_STAGE_TRACE_LEASE_MAX, observed.held_leases);
    TEST_ASSERT_EQUAL_UINT64(JCE_INSTANCE_STAGE_TRACE_LEASE_MAX, observed.peak);
    TEST_ASSERT_EQUAL_UINT64(1, observed.unavailable_total);
    TEST_ASSERT_TRUE(observed.unavailable && observed.pool_full && observed.instrumented);
    TEST_ASSERT_EQUAL_UINT64(JCE_INSTANCE_STAGE_TRACE_LEASE_MAX, observed.capacity);
    for (uint32_t i = 0; i < fake.deferred_count; ++i) {
        release_deferred(i);
        observe_stage_trace(&cursor, &observed);
    }
    TEST_ASSERT_EQUAL_UINT64(0, observed.held_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, observed.held_leases);
    TEST_ASSERT_EQUAL_UINT64(observed.transfers, observed.releases);
    fake.defer_release = false;
    JceInstanceUpload reused = reserve_dynamic(1u, 32u);
    commit_all(&reused);
    observe_stage_trace(&cursor, &observed);
    TEST_ASSERT_EQUAL_UINT32(JCE_INSTANCE_STAGE_TRACE_LEASE_MAX + 1u, fake.nonnull_leases);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_calls);
    jce_instance_upload_owner_destroy(&owner);
    observe_stage_trace(&cursor, &observed);
    TEST_ASSERT_EQUAL_UINT64(0, observed.held_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, observed.held_leases);
    TEST_ASSERT_EQUAL_UINT64(1, observed.unavailable_total);
    TEST_ASSERT_EQUAL_UINT64(JCE_INSTANCE_STAGE_TRACE_LEASE_MAX, observed.peak);
    TEST_ASSERT_EQUAL_UINT64(1, observed.pool_full_total);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_freed);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
}

static void trace_failed_update_cancels_untransferred_lease_and_staging_once(void)
{
    use_traced_provider(true);
    JceInstanceUpload upload = reserve_dynamic(4u, 80u);
    const JceInstanceUpload identity = upload;
    fake.update_fails = true;
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &upload, 2u));
    TEST_ASSERT_EQUAL_UINT32(1, fake.nonnull_leases);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_freed);
    TEST_ASSERT_EQUAL_UINT32(0, fake.backend_freed);
    TEST_ASSERT_EQUAL_UINT32(0, owner.owned_bytes);
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t cancel = token_scope(events, count, "renderer.instance.staging_cancel",
        identity.serial, "instance.staging_was_transferred", 0u);
    assert_upload_identity(events, count, cancel, &identity, 2u, NULL);
    assert_scoped_counter(events, count, cancel, identity.serial, "instance.staging_cancel_bytes", 320u);
    assert_scoped_counter(events, count, cancel, identity.serial, "instance.traced_backend_held_bytes", 0u);
    const uint64_t commit = token_scope(events, count, "renderer.instance.upload",
        identity.serial, "instance.serial", identity.serial);
    const JceTraceEvent *submit = trace_scope_event(events, count, cancel, JCE_TRACE_EVENT_TASK_SUBMIT);
    TEST_ASSERT_NOT_NULL(submit);
    TEST_ASSERT_EQUAL_UINT64(commit, submit->parent_id);
    assert_scoped_counter(events, count, commit, identity.serial, "instance.attempted_actual", 2u);
    const uint32_t updates_before = fake.updates;
    JceInstanceUpload excessive = reserve_dynamic(4u, 80u);
    const JceInstanceUpload excessive_identity = excessive;
    TEST_ASSERT_FALSE(jce_instance_upload_commit(&owner, &excessive, 5u));
    TEST_ASSERT_EQUAL_UINT32(updates_before, fake.updates);
    TEST_ASSERT_EQUAL_UINT32(0, lane_for(80u)->cursor);
    TEST_ASSERT_FALSE(owner.pending_valid);
    events = capture_trace(&count);
    const uint64_t overcount = token_scope(events, count, "renderer.instance.upload",
        excessive_identity.serial, "instance.attempted_actual", 5u);
    assert_upload_identity(events, count, overcount, &excessive_identity, 0u, NULL);
    const JceTraceEvent *failed_end = trace_scope_event(events, count, overcount, JCE_TRACE_EVENT_TASK_END);
    TEST_ASSERT_NOT_NULL(failed_end);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, failed_end->value_u32[0]);
    jce_instance_upload_owner_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_freed);
}

typedef struct AsyncStageRelease {
    JceSemaphore *start;
    FakeDeferred callbacks[2];
    uint64_t thread_id;
    uint32_t threads_before, threads_after;
    bool foreign;
} AsyncStageRelease;

static void release_stages_on_backend_thread(void *arg)
{
    AsyncStageRelease *release = (AsyncStageRelease *)arg;
    jce_semaphore_wait(release->start);
    release->thread_id = jce_thread_current_id();
    if (release->foreign) jce_trace_thread_unregister();
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    release->threads_before = stats.active_threads;
    for (uint32_t i = 0; i < 2u; ++i) {
        JCE_FREE(release->callbacks[i].data);
        jce_instance_upload_trace_release(release->callbacks[i].lease);
    }
    jce_trace_get_stats(&stats);
    release->threads_after = stats.active_threads;
}

static void run_async_trace_release(bool foreign)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
    jce_trace_thread_register("instance-trace-test-main");
    JceTraceStats baseline;
    jce_trace_get_stats(&baseline);
    TEST_ASSERT_EQUAL_UINT32(1, baseline.active_threads);
    jce_trace_thread_unregister();
    jce_trace_get_stats(&baseline);
    TEST_ASSERT_EQUAL_UINT32(0, baseline.active_threads);
    fake.defer_release = true;
    JceInstanceUploadOwner *ephemeral = (JceInstanceUploadOwner *)JCE_MALLOC(sizeof *ephemeral);
    TEST_ASSERT_NOT_NULL(ephemeral);
    jce_instance_upload_owner_init(ephemeral, &traced_provider, &fake, 0u, 29u);
    JceInstanceUpload first, second;
    TEST_ASSERT_TRUE(jce_instance_upload_begin(ephemeral, TEST_EPOCH, 4u, 64u, 128u, &first));
    first.data[0] = 1.0f;
    TEST_ASSERT_TRUE(jce_instance_upload_commit(ephemeral, &first, 2u));
    TEST_ASSERT_TRUE(jce_instance_upload_begin(ephemeral, TEST_EPOCH, 2u, 80u, 128u, &second));
    second.data[0] = 1.0f;
    TEST_ASSERT_TRUE(jce_instance_upload_commit(ephemeral, &second, 1u));
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t transfer = token_scope(events, count, "renderer.instance.staging_transfer",
        first.serial, "instance.staging_transfer_bytes", 256u);
    assert_scoped_counter(events, count, transfer, first.serial, "instance.staging_submit_bytes", 128u);
    const uint64_t first_commit = token_scope(events, count, "renderer.instance.upload",
        first.serial, "instance.token_generation", 29u);
    AsyncStageRelease release;
    memset(&release, 0, sizeof release);
    release.foreign = foreign;
    release.start = jce_semaphore_create(0u);
    TEST_ASSERT_NOT_NULL(release.start);
    memcpy(release.callbacks, fake.deferred, sizeof release.callbacks);
    JceThread *thread = jce_thread_create(release_stages_on_backend_thread, &release,
                                         "instance-stage-release-test");
    TEST_ASSERT_NOT_NULL(thread);
    jce_instance_upload_owner_destroy(ephemeral);
    JCE_FREE(ephemeral);
    const uint32_t metadata_before_release = fake.metadata_freed;
    const uint32_t staging_before_release = fake.staging_live;
    jce_semaphore_signal(release.start);
    jce_thread_join(thread);
    jce_semaphore_destroy(release.start);
    TEST_ASSERT_EQUAL_UINT32(0, metadata_before_release);
    TEST_ASSERT_EQUAL_UINT32(2, staging_before_release);
    TEST_ASSERT_TRUE(release.thread_id != jce_thread_current_id());
    TEST_ASSERT_EQUAL_UINT32(foreign ? 0u : 1u, release.threads_before);
    TEST_ASSERT_EQUAL_UINT32(foreign ? 0u : 1u, release.threads_after);
    for (uint32_t i = 0; i < 2u; ++i) {
        FakeStaging *stage = find_staging(&fake, fake.deferred[i].data);
        stage->data = NULL; stage->bytes = 0;
        fake.deferred[i].data = NULL; fake.deferred[i].lease = NULL;
        ++fake.backend_freed; --fake.staging_live;
    }
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_freed);
    TEST_ASSERT_EQUAL_UINT32(0, fake.metadata_live);
    events = capture_trace(&count);
    const uint64_t callback = token_scope(events, count, "renderer.instance.staging_release",
        first.serial, "instance.token_generation", 29u);
    assert_upload_identity(events, count, callback, &first, 2u, NULL);
    const JceTraceEvent *callback_begin = trace_scope_event(events, count, callback,
        JCE_TRACE_EVENT_TASK_BEGIN);
    TEST_ASSERT_NOT_NULL(callback_begin);
    TEST_ASSERT_TRUE(callback_begin->thread_id != 0u);
    TEST_ASSERT_EQUAL_UINT64(release.thread_id, callback_begin->thread_id);
    assert_scoped_counter(events, count, callback, first.serial, "instance.owner_observed", 0u);
    assert_scoped_counter(events, count, callback, first.serial, "instance.owner_generation", 29u);
    assert_scoped_counter(events, count, callback, first.serial, "instance.is_main", 0u);
    assert_scoped_counter(events, count, callback, first.serial, "instance.staging_release_bytes", 256u);
    assert_scoped_counter(events, count, callback, first.serial, "instance.traced_backend_held_bytes", 160u);
    const JceTraceEvent *submit = trace_scope_event(events, count, callback, JCE_TRACE_EVENT_TASK_SUBMIT);
    TEST_ASSERT_NOT_NULL(submit);
    TEST_ASSERT_EQUAL_UINT64(first_commit, submit->parent_id);
    assert_scoped_counter(events, count, callback, first.serial, "instance.origin_same_capture", 1u);
    assert_scoped_counter(events, count, callback, first.serial, "instance.staging_origin_capture_gap", 0u);
    const uint64_t last = token_scope(events, count, "renderer.instance.staging_release",
        second.serial, "instance.token_generation", 29u);
    assert_upload_identity(events, count, last, &second, 1u, NULL);
    assert_scoped_counter(events, count, last, second.serial, "instance.traced_backend_held_bytes", 0u);
    assert_scoped_counter(events, count, last, second.serial, "instance.staging_release_bytes", 160u);
    const uint64_t detach = token_scope(events, count, "renderer.instance.staging_detach",
        0u, "instance.token_generation", 29u);
    assert_scoped_counter(events, count, detach, 0u, "instance.traced_backend_held_bytes", 416u);
    assert_scoped_counter(events, count, detach, 0u, "instance.backend_held_lease_count", 2u);
    const uint64_t summary = token_scope(events, count, "renderer.instance.staging_summary",
        0u, "instance.token_generation", 29u);
    assert_scoped_counter(events, count, summary, 0u, "instance.traced_backend_held_bytes", 0u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_lease_count", 0u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_lease_peak", 2u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_unavailable_total", 0u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_instrumented_scope", 1u);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_threads);
    uint32_t registers = 0u, unregisters = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        if (events[i].thread_id != release.thread_id) continue;
        if (events[i].type == JCE_TRACE_EVENT_THREAD_REGISTER) ++registers;
        if (events[i].type == JCE_TRACE_EVENT_THREAD_UNREGISTER) ++unregisters;
    }
    TEST_ASSERT_EQUAL_UINT32(1u, registers);
    TEST_ASSERT_EQUAL_UINT32(registers, unregisters);
}

static void trace_callbacks_after_freed_owner_keep_origin_bytes_identity_and_async_wrappers(void)
{
    run_async_trace_release(false);
}

static void trace_foreign_callbacks_leave_unregistered_thread_untracked(void)
{
    run_async_trace_release(true);
}


static void trace_late_release_after_new_capture_rejects_reused_origin_span(void)
{
    use_traced_provider(true);
    fake.defer_release = true;
    JceInstanceUpload upload = reserve_dynamic(4u, 64u);
    TEST_ASSERT_TRUE(jce_instance_upload_commit(&owner, &upload, 2u));
    TEST_ASSERT_EQUAL_UINT32(2, lane_for(64u)->cursor);
    TEST_ASSERT_EQUAL_UINT32(1, fake.deferred_count);
    uint32_t count;
    JceTraceEvent *events = capture_trace(&count);
    const uint64_t origin_span = token_scope(events, count, "renderer.instance.upload",
        upload.serial, "instance.token_generation", 12u);
    const uint64_t transfer = token_scope(events, count, "renderer.instance.staging_transfer",
        upload.serial, "instance.staging_transfer_bytes", 256u);
    uint64_t origin_generation = 0u;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER && events[i].id == upload.serial &&
            events[i].parent_id == transfer &&
            strcmp(events[i].name, "instance.origin_capture_generation") == 0)
            origin_generation = events[i].value_u64;
    TEST_ASSERT_TRUE(origin_generation != 0u);
    assert_scoped_counter(events, count, transfer, upload.serial, "instance.origin_span", origin_span);
    assert_scoped_counter(events, count, transfer, upload.serial, "instance.origin_same_capture", 1u);
    assert_scoped_counter(events, count, transfer, upload.serial, "instance.staging_origin_capture_gap", 0u);
    jce_instance_upload_owner_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT32(0, fake.metadata_freed);
    TEST_ASSERT_EQUAL_UINT32(1, fake.staging_live);
    jce_trace_set_enabled(false);
    jce_trace_reset();
    jce_trace_set_enabled(true);
    TEST_ASSERT_TRUE(origin_span > 0u && origin_span <= 64u);
    uint64_t reused = 0u;
    for (uint64_t i = 0u; i < origin_span; ++i) reused = jce_trace_next_id();
    TEST_ASSERT_EQUAL_UINT64(origin_span, reused);
    jce_trace_task_submit(reused, 0u, JCE_TRACE_TASK_SERVICE, "new-capture-reused-id");
    jce_trace_task_begin(reused, reused, "new-capture-reused-id", 0u);
    jce_trace_task_end(reused, reused, "new-capture-reused-id", JCE_TRACE_TASK_SUCCEEDED, 0u);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    release_deferred(0u);
    TEST_ASSERT_EQUAL_UINT32(1, fake.backend_freed);
    TEST_ASSERT_EQUAL_UINT32(1, fake.metadata_freed);
    TEST_ASSERT_EQUAL_UINT32(0, fake.metadata_live);
    events = capture_trace(&count);
    TEST_ASSERT_NOT_NULL(trace_scope_event(events, count, reused, JCE_TRACE_EVENT_TASK_BEGIN));
    const uint64_t callback = token_scope(events, count, "renderer.instance.staging_release",
        upload.serial, "instance.token_generation", 12u);
    assert_upload_identity(events, count, callback, &upload, 2u, NULL);
    assert_scoped_counter(events, count, callback, upload.serial, "instance.owner_observed", 0u);
    assert_scoped_counter(events, count, callback, upload.serial, "instance.origin_span", origin_span);
    assert_scoped_counter(events, count, callback, upload.serial,
                          "instance.origin_capture_generation", origin_generation);
    assert_scoped_counter(events, count, callback, upload.serial, "instance.origin_same_capture", 0u);
    assert_scoped_counter(events, count, callback, upload.serial, "instance.staging_origin_capture_gap", 1u);
    assert_scoped_counter(events, count, callback, upload.serial, "instance.staging_release_bytes", 256u);
    assert_scoped_counter(events, count, callback, upload.serial, "instance.traced_backend_held_bytes", 0u);
    uint64_t current_generation = 0u;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER && events[i].id == upload.serial &&
            events[i].parent_id == callback &&
            strcmp(events[i].name, "instance.current_capture_generation") == 0)
            current_generation = events[i].value_u64;
    TEST_ASSERT_TRUE(current_generation != 0u && current_generation != origin_generation);
    const JceTraceEvent *submit = trace_scope_event(events, count, callback, JCE_TRACE_EVENT_TASK_SUBMIT);
    TEST_ASSERT_NOT_NULL(submit);
    TEST_ASSERT_EQUAL_UINT64(0, submit->parent_id);
    const uint64_t summary = token_scope(events, count, "renderer.instance.staging_summary",
        0u, "instance.token_generation", 12u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_origin_capture_gap", 1u);
    assert_scoped_counter(events, count, summary, 0u, "instance.staging_lease_count", 0u);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_threads);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(zero_handles_are_valid_and_candidates_publish_only_on_commit);
    RUN_TEST(qualified_32_64_80_families_share_one_128_mib_budget);
    RUN_TEST(pending_candidates_and_retired_buffers_remain_inside_shared_budget);
    RUN_TEST(full_retired_ring_blocks_replacement_but_allows_unused_family_creation);
    RUN_TEST(geometric_growth_respects_each_callers_smaller_budget);
    RUN_TEST(smaller_budget_preserves_current_slices_then_trims_at_epoch_edge);
    RUN_TEST(transient_admission_uses_actual_num_and_byte_size);
    RUN_TEST(short_complete_fit_transient_retries_full_dynamic_and_restores_partial_after_create_failure);
    RUN_TEST(invalid_transient_allocation_uses_dynamic_fallback);
    RUN_TEST(explicit_transient_reserve_keeps_headroom_for_later_draws);
    RUN_TEST(partial_transient_is_last_fallback_and_never_consumes_reserved_headroom);
    RUN_TEST(one_pending_reservation_rejects_overlapping_begin);
    RUN_TEST(actual_commit_counts_produce_disjoint_same_epoch_slices);
    RUN_TEST(staging_allocation_failure_publishes_nothing_and_preserves_cursor);
    RUN_TEST(create_failure_releases_staging_without_publishing_candidate);
    RUN_TEST(failed_candidate_update_cancels_once_and_keeps_previous_lane);
    RUN_TEST(failed_first_update_and_existing_lane_update_consume_no_cursor);
    RUN_TEST(discard_aliases_and_zero_commit_release_each_resource_once);
    RUN_TEST(overcount_commit_discards_without_backend_update);
    RUN_TEST(epoch_advance_cancels_pending_invalidates_tokens_and_resets_cursors);
    RUN_TEST(explicit_context_generation_rejects_zero_and_stale_reused_handles);
    RUN_TEST(uint32_submission_epoch_wrap_collects_once_and_invalidates_old_tokens);
    RUN_TEST(growth_retains_old_slices_until_actual_submission_epoch_changes);
    RUN_TEST(committed_encoder_bind_is_read_only_and_rejects_invalid_counts);
    RUN_TEST(destroy_releases_active_retired_and_pending_resources_once);
    RUN_TEST(overflowed_counts_unsupported_strides_and_zero_budgets_fail_without_allocation);
    RUN_TEST(opt_in_trace_reports_balanced_lifetime_scopes_counters_and_update_failure);
    RUN_TEST(trace_off_uses_no_metadata_or_trace_lease);
    RUN_TEST(trace_same_epoch_tokens_and_encoders_have_distinct_scoped_identities);
    RUN_TEST(trace_reused_handle_preserves_allocation_and_stale_owner_identity);
    RUN_TEST(trace_rejected_partial_token_keeps_requested_count_and_current_epoch_separate);
    RUN_TEST(trace_candidate_cancel_retire_and_shutdown_keep_original_resource_identity);
    RUN_TEST(trace_dynamic_recovery_retains_transient_discrepancy_reason);
    RUN_TEST(trace_metadata_oom_keeps_upload_success_and_reports_capture_gap);
    RUN_TEST(trace_lease_exhaustion_preserves_rendering_and_streams_incomplete_footer);
    RUN_TEST(trace_failed_update_cancels_untransferred_lease_and_staging_once);
    RUN_TEST(trace_callbacks_after_freed_owner_keep_origin_bytes_identity_and_async_wrappers);
    RUN_TEST(trace_foreign_callbacks_leave_unregistered_thread_untracked);
    RUN_TEST(trace_late_release_after_new_capture_rejects_reused_origin_span);
    return UNITY_END();
}
