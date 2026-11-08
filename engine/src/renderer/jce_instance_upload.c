/* One renderer-owned, provider-driven CPU instance upload service. */
#include "jce_instance_upload.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_thread.h>
#include <string.h>


static int lane_index(uint16_t stride)
{
    return stride == 32u ? 0 : stride == 64u ? 1 : stride == 80u ? 2 : -1;
}

static void trace_state(const JceInstanceUploadOwner *owner, uint32_t requested,
                         uint32_t admitted, uint16_t stride, bool error)
{
    if (!jce_trace_enabled()) return;
    uint32_t retired = 0;
    uint32_t candidate = 0;
    if (owner) {
        for (uint32_t i = 0; i < owner->retired_count; ++i)
            retired += owner->retired[i].bytes;
        if (owner->pending_valid && owner->pending.candidate)
            candidate = owner->pending.capacity * owner->pending.stride;
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.logical_owned_bytes", owner->owned_bytes);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.logical_live_bytes", owner->owned_bytes - retired - candidate);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.logical_candidate_bytes", candidate);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.pending_staging_bytes",
            owner->pending_valid && owner->pending.dynamic ? owner->pending.count * owner->pending.stride : 0u);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.error_reason", error ? owner->last_error : JCE_INSTANCE_ERROR_NONE);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.logical_retired_bytes", retired);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.generation", (int64_t)owner->generation);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.submission_epoch", owner->epoch);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.epoch_serial", (int64_t)owner->epoch_serial);
    }
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.requested", requested);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.admitted", admitted);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.stride", stride);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.error", error ? 1 : 0);
}

static JceInstanceUploadIdentity upload_identity(const JceInstanceUpload *upload)
{
    JceInstanceUploadIdentity id;
    memset(&id, 0, sizeof id);
    if (!upload) return id;
    id.generation = upload->generation; id.serial = upload->serial;
    id.epoch = upload->epoch; id.epoch_serial = upload->epoch_serial;
    id.allocation_serial = upload->allocation_serial;
    id.requested = upload->requested; id.count = upload->count; id.actual = upload->actual;
    id.offset = upload->offset; id.capacity = upload->dynamic ? upload->capacity : upload->transient.num;
    id.stride = upload->stride; id.handle = upload->dynamic ? upload->handle : upload->transient.handle;
    id.dynamic = upload->dynamic; id.transient_offset = upload->transient.offset;
    id.transient_size = upload->transient.size; id.transient_num = upload->transient.num;
    id.transient_stride = upload->transient.stride;
    id.fallback_reason = upload->fallback_reason;
    return id;
}

static void trace_identity(const JceInstanceUploadOwner *owner,
    const JceInstanceUploadIdentity *id, uint32_t actual, void *encoder)
{
    if (!jce_trace_enabled() || !id) return;
    const uint64_t parent = jce_trace_task_current_id();
    const bool observed = owner && owner->initialized;
#define IC(name_, value_) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, \
    "instance." name_, (int64_t)(value_), id->serial, parent)
    IC("owner_observed", observed ? 1 : 0);
    IC("owner_generation", observed ? owner->generation : id->generation);
    IC("owner_epoch", observed ? owner->epoch : id->epoch);
    IC("owner_epoch_serial", observed ? owner->epoch_serial : id->epoch_serial);
    IC("token_generation", id->generation); IC("serial", id->serial);
    IC("allocation_serial", id->allocation_serial);
    IC("epoch", id->epoch); IC("epoch_serial", id->epoch_serial);
    IC("handle", id->handle); IC("offset", id->offset);
    IC("requested", id->requested); IC("count", id->count); IC("actual", actual);
    IC("capacity", id->capacity); IC("stride", id->stride);
    IC("route_dynamic", id->dynamic ? 1 : 0); IC("fallback_reason", id->fallback_reason);
    IC("transient_offset", id->transient_offset); IC("transient_size", id->transient_size);
    IC("transient_num", id->transient_num); IC("transient_stride", id->transient_stride);
    IC("encoder_opaque", (uint64_t)(uintptr_t)encoder);
    IC("is_main", jce_thread_is_main() ? 1 : 0);
#undef IC
}

static void trace_resource(JceInstanceUploadOwner *owner,
    const JceInstanceUploadIdentity *id, const char *name, uint32_t reason, JceTraceTaskState result)
{
    if (!jce_trace_enabled()) return;
    JceTraceScope scope = jce_trace_scope_begin(name);
    trace_identity(owner, id, id->actual, NULL);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.resource_reason",
        reason, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.resource_capacity_bytes",
        (int64_t)id->capacity * id->stride, id->serial, scope.id);
    jce_trace_scope_end(scope, result);
}

static void destroy_resource(JceInstanceUploadOwner *owner,
    const JceInstanceUploadIdentity *id, uint32_t reason)
{
    JceTraceScope scope = jce_trace_scope_begin("renderer.instance.handle_destroy");
    trace_identity(owner, id, id->actual, NULL);
    if (scope.id) {
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.resource_reason",
            reason, id->serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.resource_capacity_bytes",
            (int64_t)id->capacity * id->stride, id->serial, scope.id);
    }
    owner->provider->destroy_dynamic(owner->user, id->handle);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

/* Only opt-in trace state owns these fixed slots. Their immutable snapshots
 * and copied allocator context outlive the renderer owner through references.
 * The held gauge covers instrumented CPU staging, not physical GPU resources. */
struct JceInstanceStageLease {
    JceInstanceStageTrace *state;
    JceInstanceUploadIdentity identity;
    uint64_t origin_span;
    uint32_t bytes, capture_generation;
    bool occupied, transferred;
};
struct JceInstanceStageTrace {
    JceAtomicI32 *refs;
    JceAtomicU64 *held_bytes;
    JceMutex *guard;
    void (*free_metadata)(void *, void *);
    void *allocator_user;
    uint64_t generation, origin_span, unavailable_total, unavailable_reason[6];
    uint32_t lease_count, lease_peak, backend_count, backend_peak, capture_generation;
    JceInstanceStageLease leases[JCE_INSTANCE_STAGE_TRACE_LEASE_MAX];
};

static void default_trace_free(void *user, void *data)
{
    (void)user;
    JCE_FREE(data);
}
static void stage_snapshot(JceInstanceStageTrace *state,
    const JceInstanceUploadIdentity *id, uint64_t parent)
{
    if (!jce_trace_enabled() || !state) return;
    uint32_t count, peak, backend, backend_peak;
    uint64_t held, unavailable, reasons[6];
    jce_mutex_lock(state->guard);
    count = state->lease_count; peak = state->lease_peak;
    backend = state->backend_count; backend_peak = state->backend_peak;
    held = jce_atomic_u64_load(state->held_bytes);
    unavailable = state->unavailable_total;
    memcpy(reasons, state->unavailable_reason, sizeof reasons);
    jce_mutex_unlock(state->guard);
#define SC(name_, value_) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, \
    "instance." name_, (int64_t)(value_), id->serial, parent)
    SC("staging_slot_capacity", JCE_INSTANCE_STAGE_TRACE_LEASE_MAX);
    SC("staging_lease_count", count); SC("staging_lease_peak", peak);
    SC("backend_held_lease_count", backend); SC("backend_held_lease_peak", backend_peak);
    SC("traced_backend_held_bytes", held); SC("staging_unavailable_total", unavailable);
    SC("staging_state_oom_total", reasons[1]); SC("staging_atomic_oom_total", reasons[2]);
    SC("staging_mutex_oom_total", reasons[3]); SC("staging_pool_full_total", reasons[4]);
    SC("staging_provider_gap_total", reasons[5]);
    /* All bytes/leases above are instrumented CPU staging only. Capture gaps
     * (including ring overwrite reported by the core cursor) preclude a claim
     * of complete resource tracing; these counters never certify GPU completion. */
    SC("staging_instrumented_scope", 1);
#undef SC
}
/* A saved span ID belongs to one capture. Never link a late callback to a
 * numerically reused span in a later capture; retain the immutable origin and
 * expose the gap instead. This does not change backend/resource ownership. */
static JceTraceScope stage_scope(const char *name, const JceInstanceUploadIdentity *id,
    uint64_t origin, uint32_t generation)
{
    if (!jce_trace_enabled()) { JceTraceScope off = {0}; return off; }
    const uint32_t current = jce_trace_capture_generation();
    JceTraceScope scope = jce_trace_scope_begin_parent(name, current == generation ? origin : 0u);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.origin_capture_generation",
        generation, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.current_capture_generation",
        current, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.origin_span",
        (int64_t)origin, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.origin_same_capture",
        current == generation ? 1 : 0, id->serial, scope.id);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_origin_capture_gap",
        current != generation ? 1 : 0, id->serial, scope.id);
    return scope;
}

static void stage_summary(JceInstanceStageTrace *state, const char *name)
{
    if (!jce_trace_enabled() || !state) return;
    JceInstanceUploadIdentity id;
    memset(&id, 0, sizeof id);
    id.generation = state->generation; id.handle = JCE_INSTANCE_UPLOAD_INVALID;
    JceTraceScope scope = stage_scope(name, &id, state->origin_span, state->capture_generation);
    trace_identity(NULL, &id, 0, NULL);
    stage_snapshot(state, &id, scope.id);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

static void stage_state_unref(JceInstanceStageTrace *state)
{
    if (!state || jce_atomic_i32_add(state->refs, -1) != 1) return;
    stage_summary(state, "renderer.instance.staging_summary");
    jce_mutex_destroy(state->guard);
    jce_atomic_u64_destroy(state->held_bytes);
    jce_atomic_i32_destroy(state->refs);
    state->free_metadata(state->allocator_user, state);
}
static void stage_unavailable(JceInstanceUploadOwner *owner,
    const JceInstanceUploadIdentity *id, uint32_t reason)
{
    if (!jce_trace_enabled()) return;
    if (owner) {
        ++owner->stage_unavailable_total;
        if (reason < 6u) ++owner->stage_unavailable_reason[reason];
        if (owner->stage_trace) {
            JceInstanceStageTrace *state = owner->stage_trace;
            jce_mutex_lock(state->guard);
            state->unavailable_total = owner->stage_unavailable_total;
            memcpy(state->unavailable_reason, owner->stage_unavailable_reason, sizeof state->unavailable_reason);
            jce_mutex_unlock(state->guard);
        }
    }
    trace_identity(owner, id, id->actual, NULL);
    const uint64_t parent = jce_trace_task_current_id();
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_trace_unavailable",
        1, id->serial, parent);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_trace_reason",
        reason, id->serial, parent);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_unavailable_total",
        owner ? (int64_t)owner->stage_unavailable_total : 1, id->serial, parent);
}

JceInstanceStageLease *jce_instance_upload_trace_prepare(JceInstanceUploadOwner *owner,
    const JceInstanceUploadIdentity *id, uint32_t bytes)
{
    if (!jce_trace_enabled()) return NULL;
    if (!owner || !owner->initialized || !id || !bytes) return NULL;
    JceInstanceStageTrace *state = owner->stage_trace;
    if (!state) {
        const bool custom = owner->provider->allocate_trace_metadata && owner->provider->free_trace_metadata;
        state = custom ? (JceInstanceStageTrace *)owner->provider->allocate_trace_metadata(owner->user, sizeof *state)
                       : (JceInstanceStageTrace *)JCE_MALLOC(sizeof *state);
        if (!state) { stage_unavailable(owner, id, 1u); return NULL; }
        memset(state, 0, sizeof *state);
        state->free_metadata = custom ? owner->provider->free_trace_metadata : default_trace_free;
        state->allocator_user = custom ? owner->user : NULL;
        state->generation = id->generation; state->origin_span = jce_trace_task_current_id();
        state->capture_generation = jce_trace_capture_generation();
        state->unavailable_total = owner->stage_unavailable_total;
        memcpy(state->unavailable_reason, owner->stage_unavailable_reason, sizeof state->unavailable_reason);
        state->refs = jce_atomic_i32_create(1);
        state->held_bytes = jce_atomic_u64_create(0);
        state->guard = jce_mutex_create();
        if (!state->refs || !state->held_bytes || !state->guard) {
            const uint32_t reason = !state->refs || !state->held_bytes ? 2u : 3u;
            if (state->refs) jce_atomic_i32_destroy(state->refs);
            if (state->held_bytes) jce_atomic_u64_destroy(state->held_bytes);
            if (state->guard) jce_mutex_destroy(state->guard);
            state->free_metadata(state->allocator_user, state);
            stage_unavailable(owner, id, reason);
            return NULL;
        }
        owner->stage_trace = state;
    }
    jce_mutex_lock(state->guard);
    JceInstanceStageLease *lease = NULL;
    for (uint32_t i = 0; i < JCE_INSTANCE_STAGE_TRACE_LEASE_MAX; ++i) {
        if (!state->leases[i].occupied) { lease = &state->leases[i]; break; }
    }
    if (lease) {
        lease->state = state; lease->identity = *id;
        lease->origin_span = jce_trace_task_current_id();
        lease->capture_generation = jce_trace_capture_generation();
        lease->bytes = bytes; lease->occupied = true; lease->transferred = false;
        ++state->lease_count;
        if (state->lease_count > state->lease_peak) state->lease_peak = state->lease_count;
        jce_atomic_i32_add(state->refs, 1);
    }
    jce_mutex_unlock(state->guard);
    if (!lease) stage_unavailable(owner, id, 4u);
    return lease;
}

void jce_instance_upload_trace_transfer(JceInstanceStageLease *lease)
{
    if (!lease) return;
    JceInstanceStageTrace *state = lease->state;
    jce_atomic_i32_add(state->refs, 1);
    jce_mutex_lock(state->guard);
    if (!lease->occupied || lease->transferred) {
        jce_mutex_unlock(state->guard); stage_state_unref(state); return;
    }
    const JceInstanceUploadIdentity id = lease->identity;
    const uint64_t origin = lease->origin_span;
    const uint32_t capture_generation = lease->capture_generation;
    const uint32_t bytes = lease->bytes;
    lease->transferred = true;
    ++state->backend_count;
    if (state->backend_count > state->backend_peak) state->backend_peak = state->backend_count;
    /* Charge before provider can dispatch a synchronous/concurrent callback. */
    jce_atomic_u64_add(state->held_bytes, bytes);
    jce_mutex_unlock(state->guard);
    JceTraceScope scope = stage_scope("renderer.instance.staging_transfer", &id, origin, capture_generation);
    trace_identity(NULL, &id, id.actual, NULL);
    if (scope.id) {
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_transfer_bytes",
            bytes, id.serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_submit_bytes",
            (int64_t)id.actual * id.stride, id.serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.traced_backend_held_bytes",
            (int64_t)jce_atomic_u64_load(state->held_bytes), id.serial, scope.id);
        stage_snapshot(state, &id, scope.id);
    }
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
    stage_state_unref(state);
}

static void finish_stage_lease(JceInstanceStageLease *lease, bool released)
{
    if (!lease) return;
    JceInstanceStageTrace *state = lease->state;
    jce_mutex_lock(state->guard);
    if (!lease->occupied) { jce_mutex_unlock(state->guard); return; }
    const JceInstanceUploadIdentity id = lease->identity;
    const uint64_t parent = lease->origin_span;
    const uint32_t capture_generation = lease->capture_generation;
    const uint32_t bytes = lease->bytes;
    const bool transferred = lease->transferred;
    if (transferred) {
        jce_atomic_u64_add(state->held_bytes, UINT64_C(0) - bytes);
        --state->backend_count;
    }
    --state->lease_count;
    lease->occupied = false;
    const uint64_t held = jce_atomic_u64_load(state->held_bytes);
    jce_mutex_unlock(state->guard);
    JceTraceScope scope = stage_scope(released ? "renderer.instance.staging_release"
                                              : "renderer.instance.staging_cancel", &id, parent, capture_generation);
    trace_identity(NULL, &id, id.actual, NULL);
    if (scope.id) {
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            released ? "instance.staging_release_bytes" : "instance.staging_cancel_bytes",
            bytes, id.serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_was_transferred",
            transferred ? 1 : 0, id.serial, scope.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.traced_backend_held_bytes",
            (int64_t)held, id.serial, scope.id);
        stage_snapshot(state, &id, scope.id);
    }
    jce_trace_scope_end(scope, released ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_CANCELLED);
    stage_state_unref(state);
}
void jce_instance_upload_trace_release(JceInstanceStageLease *lease)
{
    if (!lease) return;
    /* Counter/span emission stamps the callback thread without registering it.
     * Existing JCE registrations are preserved; foreign threads incur no debt. */
    finish_stage_lease(lease, true);
}
void jce_instance_upload_trace_cancel(JceInstanceStageLease *lease)
{
    finish_stage_lease(lease, false);
}

static void trace_rejected(const JceInstanceUploadOwner *owner, uint32_t requested,
                            uint16_t stride, const char *name, JceInstanceUploadError reason,
                            const JceInstanceUpload *upload, void *encoder, uint32_t attempted)
{
    if (!jce_trace_enabled()) return;
    JceTraceScope scope = jce_trace_scope_begin(name);
    trace_state(owner && owner->initialized ? owner : NULL, requested, 0, stride, true);
    JceInstanceUploadIdentity id = upload_identity(upload);
    if (!upload || !upload->serial) { id.requested = requested; id.stride = stride; }
    trace_identity(owner, &id, 0, encoder);
    jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.attempted_actual",
        attempted, id.serial, scope.id);
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.error_reason", reason);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
}

static void warn_partial(JceInstanceUploadOwner *owner, uint32_t requested,
                          uint32_t admitted, uint16_t stride)
{
    if (!owner->warned || (uint32_t)(owner->epoch - owner->warned_epoch) >= 120u) {
        owner->warned = true;
        owner->warned_epoch = owner->epoch;
        if (owner->provider->warn_partial)
            owner->provider->warn_partial(owner->user, owner->epoch, requested, admitted, stride);
    }
}

static void cancel_pending(JceInstanceUploadOwner *owner, uint32_t reason)
{
    if (!owner->pending_valid) return;
    JceInstanceUpload *pending = &owner->pending;
    const JceInstanceUploadIdentity id = upload_identity(pending);
    JceTraceScope scope = {0};
    if (reason != 4u) {
        scope = jce_trace_scope_begin("renderer.instance.implicit_cancel");
        trace_identity(owner, &id, 0, NULL);
        if (scope.id) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "instance.resource_reason", reason, id.serial, scope.id);
    }
    if (pending->dynamic) {
        if (pending->data) {
            JceTraceScope free_scope = jce_trace_scope_begin("renderer.instance.staging_cpu_free");
            trace_identity(owner, &id, 0, NULL);
            if (free_scope.id) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
                "instance.staging_free_bytes", (int64_t)pending->count * pending->stride,
                id.serial, free_scope.id);
            owner->provider->free_staging(owner->user, pending->data);
            jce_trace_scope_end(free_scope, JCE_TRACE_TASK_CANCELLED);
        }
        if (pending->candidate) {
            destroy_resource(owner, &id, reason);
            owner->owned_bytes -= pending->capacity * pending->stride;
        }
    }
    memset(pending, 0, sizeof *pending);
    owner->pending_valid = false;
    jce_trace_scope_end(scope, JCE_TRACE_TASK_CANCELLED);
}

void jce_instance_upload_owner_init(JceInstanceUploadOwner *owner,
    const JceInstanceUploadProvider *provider, void *user, uint32_t transient_reserve_bytes, uint64_t context_generation)
{
    if (!owner) return;
    memset(owner, 0, sizeof *owner);
    if (!context_generation || !provider || !provider->available || !provider->allocate_transient ||
        !provider->create_dynamic || !provider->destroy_dynamic ||
        !provider->allocate_staging || !provider->free_staging ||
        !provider->update_dynamic || !provider->bind_dynamic || !provider->bind_transient) return;
    JceTraceScope scope = jce_trace_scope_begin("renderer.instance.init");
    owner->provider = provider;
    owner->user = user;
    owner->generation = context_generation;
    for (uint32_t i = 0; i < 3u; ++i) owner->lanes[i].handle = JCE_INSTANCE_UPLOAD_INVALID;
    owner->lanes[0].stride = 32u;
    owner->lanes[1].stride = 64u;
    owner->lanes[2].stride = 80u;
    owner->transient_reserve_bytes = transient_reserve_bytes;
    owner->initialized = true;
    trace_state(owner, 0, 0, 0, false);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

void jce_instance_upload_owner_advance(JceInstanceUploadOwner *owner, uint32_t epoch)
{
    if (!owner || !owner->initialized || (owner->epoch_valid && owner->epoch == epoch)) return;
    JceTraceScope scope = jce_trace_scope_begin("renderer.instance.retire");
    cancel_pending(owner, 1u);
    /* Commands for every retired slice were submitted in the previous epoch.
     * Request destruction now; bgfx's deferred destruction owns backend safety.
     * This ordinal is not a hardware fence or a readback completion index. */
    for (uint32_t i = 0; i < owner->retired_count; ++i) {
        destroy_resource(owner, &owner->retired[i].identity, 1u);
        owner->owned_bytes -= owner->retired[i].bytes;
    }
    owner->retired_count = 0;
    ++owner->epoch_serial;
    owner->epoch = epoch;
    owner->epoch_valid = true;
    for (uint32_t i = 0; i < 3u; ++i) owner->lanes[i].cursor = 0;
    trace_state(owner, 0, 0, 0, false);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

void jce_instance_upload_owner_destroy(JceInstanceUploadOwner *owner)
{
    if (!owner || !owner->initialized) return;
    JceTraceScope scope = jce_trace_scope_begin("renderer.instance.destroy");
    cancel_pending(owner, 3u);
    for (uint32_t i = 0; i < 3u; ++i) {
        if (owner->lanes[i].handle != JCE_INSTANCE_UPLOAD_INVALID)
            destroy_resource(owner, &owner->lanes[i].identity, 3u);
    }
    for (uint32_t i = 0; i < owner->retired_count; ++i)
        destroy_resource(owner, &owner->retired[i].identity, 3u);
    owner->retired_count = 0;
    owner->owned_bytes = 0;
    trace_state(owner, 0, 0, 0, false);
    if (owner->stage_trace) {
        stage_summary(owner->stage_trace, "renderer.instance.staging_detach");
        stage_state_unref(owner->stage_trace);
        owner->stage_trace = NULL;
    } else if (jce_trace_enabled()) {
        JceInstanceUploadIdentity id;
        memset(&id, 0, sizeof id); id.generation = owner->generation;
        id.handle = JCE_INSTANCE_UPLOAD_INVALID;
        JceTraceScope summary = jce_trace_scope_begin("renderer.instance.staging_summary");
        trace_identity(owner, &id, 0, NULL);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_unavailable_total",
            (int64_t)owner->stage_unavailable_total, 0, summary.id);
        const char *reasons[6] = {"", "instance.staging_state_oom_total", "instance.staging_atomic_oom_total",
            "instance.staging_mutex_oom_total", "instance.staging_pool_full_total", "instance.staging_provider_gap_total"};
        for (uint32_t i = 1; i < 6u; ++i)
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, reasons[i],
                (int64_t)owner->stage_unavailable_reason[i], 0, summary.id);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_instrumented_scope",
            1, 0, summary.id);
        jce_trace_scope_end(summary, JCE_TRACE_TASK_SUCCEEDED);
    }
    memset(owner, 0, sizeof *owner);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

static bool reserve_transient(JceInstanceUploadOwner *owner, uint32_t count,
                               uint16_t stride, JceInstanceUpload *out)
{
    if (!count || !owner->provider->allocate_transient(owner->user, count, stride, &out->transient))
        return false;
    JceInstanceTransient *t = &out->transient;
    if (!t->data || t->stride != stride || t->handle == JCE_INSTANCE_UPLOAD_INVALID) return false;
    uint32_t actual = t->num < count ? t->num : count;
    const uint32_t byte_count = t->size / stride;
    if (actual > byte_count) actual = byte_count;
    if (!actual) return false;
    out->count = actual;
    out->data = (float *)t->data;
    return true;
}

static bool reserve_dynamic(JceInstanceUploadOwner *owner, uint32_t requested,
                             uint16_t stride, uint32_t caller_budget_mb,
                             JceInstanceUpload *out)
{
    JceInstanceUploadLane *lane = &owner->lanes[lane_index(stride)];
    const uint32_t budget = caller_budget_mb >= 128u ? JCE_INSTANCE_UPLOAD_BUDGET_BYTES
                                                    : caller_budget_mb * 1048576u;
    const uint32_t limit = budget / stride;
    if (lane->cursor > limit || requested > limit - lane->cursor) {
        owner->last_error = JCE_INSTANCE_ERROR_CALLER_LIMIT;
        return false;
    }
    const uint32_t needed = lane->cursor + requested;
    uint32_t capacity = lane->capacity;
    const bool candidate = needed > capacity;
    if (candidate) {
        if (lane->handle != JCE_INSTANCE_UPLOAD_INVALID &&
            owner->retired_count >= JCE_INSTANCE_UPLOAD_RETIRED_MAX) {
            owner->last_error = JCE_INSTANCE_ERROR_AGGREGATE_LIMIT;
            return false;
        }
        const uint32_t remaining = JCE_INSTANCE_UPLOAD_BUDGET_BYTES - owner->owned_bytes;
        uint32_t max_capacity = remaining / stride;
        if (max_capacity > limit) max_capacity = limit;
        if (needed > max_capacity) {
            owner->last_error = JCE_INSTANCE_ERROR_AGGREGATE_LIMIT;
            return false;
        }
        capacity = capacity ? capacity : 1u;
        while (capacity < needed) {
            if (capacity > max_capacity / 2u) { capacity = max_capacity; break; }
            capacity *= 2u;
        }
    }
    void *data = owner->provider->allocate_staging(owner->user, requested * stride);
    if (!data) {
        owner->last_error = JCE_INSTANCE_ERROR_STAGING;
        return false;
    }
    uint16_t handle = lane->handle;
    if (candidate) {
        handle = owner->provider->create_dynamic(owner->user, capacity, stride);
        if (handle == JCE_INSTANCE_UPLOAD_INVALID) {
            owner->last_error = JCE_INSTANCE_ERROR_CREATE;
            if (jce_trace_enabled()) {
                JceInstanceUploadIdentity id = upload_identity(out);
                id.dynamic = true; id.handle = handle; id.capacity = capacity;
                id.count = requested; id.offset = lane->cursor;
                trace_resource(owner, &id, "renderer.instance.candidate_create",
                    JCE_INSTANCE_ERROR_CREATE, JCE_TRACE_TASK_FAILED);
                JceTraceScope free_scope = jce_trace_scope_begin("renderer.instance.staging_cpu_free");
                trace_identity(owner, &id, 0, NULL);
                jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.staging_free_bytes",
                    (int64_t)requested * stride, id.serial, free_scope.id);
                owner->provider->free_staging(owner->user, data);
                jce_trace_scope_end(free_scope, JCE_TRACE_TASK_CANCELLED);
            } else owner->provider->free_staging(owner->user, data);
            return false;
        }
        owner->owned_bytes += capacity * stride;
    }
    out->dynamic = true;
    out->candidate = candidate;
    out->allocation_serial = candidate ? out->serial : lane->identity.allocation_serial;
    out->handle = handle;
    out->capacity = capacity;
    out->offset = lane->cursor;
    out->count = requested;
    out->data = (float *)data;
    if (candidate) {
        const JceInstanceUploadIdentity id = upload_identity(out);
        trace_resource(owner, &id, "renderer.instance.candidate_create", 0u, JCE_TRACE_TASK_SUCCEEDED);
    }
    return true;
}

bool jce_instance_upload_begin(JceInstanceUploadOwner *owner, uint32_t epoch,
    uint32_t requested, uint16_t stride, uint32_t caller_budget_mb, JceInstanceUpload *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!owner || !owner->initialized || lane_index(stride) < 0 || !requested ||
        requested > UINT32_MAX / stride) {
        trace_rejected(owner, requested, stride, "renderer.instance.reserve", JCE_INSTANCE_ERROR_REQUEST, out, NULL, requested);
        return false;
    }
    JceTraceScope scope = jce_trace_scope_begin("renderer.instance.reserve");
    jce_instance_upload_owner_advance(owner, epoch);
    if (owner->pending_valid) {
        owner->last_error = JCE_INSTANCE_ERROR_PENDING;
        warn_partial(owner, requested, 0u, stride);
        trace_state(owner, requested, 0, stride, true);
        if (scope.id) {
            JceInstanceUploadIdentity id = upload_identity(out);
            id.generation = owner->generation; id.epoch = epoch; id.epoch_serial = owner->epoch_serial;
            id.requested = requested; id.stride = stride; id.handle = JCE_INSTANCE_UPLOAD_INVALID;
            trace_identity(owner, &id, 0, NULL);
            jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.blocking_pending_serial",
                (int64_t)owner->pending.serial, 0, scope.id);
        }
        jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
        return false;
    }
    owner->last_error = JCE_INSTANCE_ERROR_NONE;
    /* No dynamic slice in this epoch references a lane with cursor zero.
     * A caller budget drop can therefore request its deferred destruction
     * before replacement, without invalidating current-epoch committed tokens. */
    JceInstanceUploadLane *lane = &owner->lanes[lane_index(stride)];
    const uint32_t caller_bytes = caller_budget_mb >= 128u ? JCE_INSTANCE_UPLOAD_BUDGET_BYTES
                                                         : caller_budget_mb * 1048576u;
    if (!lane->cursor && lane->capacity > caller_bytes / stride) {
        JceTraceScope trim_scope = jce_trace_scope_begin("renderer.instance.trim");
        destroy_resource(owner, &lane->identity, 2u);
        owner->owned_bytes -= lane->capacity * stride;
        lane->handle = JCE_INSTANCE_UPLOAD_INVALID;
        lane->capacity = 0;
        trace_state(owner, requested, 0, stride, false);
        jce_trace_scope_end(trim_scope, JCE_TRACE_TASK_SUCCEEDED);
    }
    out->owner = owner;
    out->generation = owner->generation;
    out->epoch = epoch;
    out->epoch_serial = owner->epoch_serial;
    out->stride = stride;
    out->handle = JCE_INSTANCE_UPLOAD_INVALID;
    out->serial = ++owner->serial;
    out->allocation_serial = out->serial;
    out->requested = requested;
    const uint32_t headroom = owner->transient_reserve_bytes / stride +
                              (owner->transient_reserve_bytes % stride != 0u ? 1u : 0u);
    const uint32_t query = headroom <= UINT32_MAX / stride &&
                           requested <= UINT32_MAX / stride - headroom
                         ? requested + headroom : requested;
    uint32_t available = owner->provider->available(owner->user, query, stride);
    if (available > query) available = query;
    const uint32_t usable = available > headroom ? available - headroom : 0u;
    bool ok = false;
    bool have_partial = false;
    JceInstanceUpload partial;
    memset(&partial, 0, sizeof partial);
    if (usable >= requested) {
        ok = reserve_transient(owner, requested, stride, out);
        if (!ok || out->count < requested) out->fallback_reason = JCE_INSTANCE_ERROR_TRANSIENT;
        if (ok && out->count < requested) {
            partial = *out;
            have_partial = true;
            ok = false;
        }
    }
    /* A complete-fit query is only a prediction. If actual num/size/stride
     * disagrees, retry the full dynamic route before accepting partial data.
     * bgfx cannot reclaim the already allocated transient slice in this epoch. */
    if (!ok) ok = reserve_dynamic(owner, requested, stride, caller_budget_mb, out);
    if (!ok && have_partial) { *out = partial; ok = true; }
    if (!ok && usable) ok = reserve_transient(owner, usable < requested ? usable : requested, stride, out);
    if (!ok || out->count != requested) warn_partial(owner, requested, ok ? out->count : 0u, stride);
    if (ok) {
        if (out->count == requested) owner->last_error = JCE_INSTANCE_ERROR_NONE;
        else if (owner->last_error == JCE_INSTANCE_ERROR_NONE) owner->last_error = JCE_INSTANCE_ERROR_TRANSIENT;
        owner->pending = *out;
        owner->pending_valid = true;
    }
    /* Capture failed reservation identity before retaining the established
     * zeroed-output failure contract. A serial was already reserved above. */
    JceInstanceUploadIdentity traced;
    const bool tracing = jce_trace_enabled();
    if (tracing) traced = upload_identity(out);
    if (!ok) memset(out, 0, sizeof *out);
    trace_state(owner, requested, ok ? out->count : 0u, stride, !ok || out->count != requested);
    if (tracing) {
        trace_identity(owner, &traced, 0u, NULL);
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.route_dynamic", ok ? (out->dynamic ? 1 : 0) : -1);
    }
    jce_trace_scope_end(scope, ok ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
    return ok;
}

static bool matches_pending(const JceInstanceUploadOwner *owner, const JceInstanceUpload *upload)
{
    return owner && owner->initialized && upload && upload->owner == owner &&
           upload->generation == owner->generation && upload->epoch == owner->epoch &&
           upload->epoch_serial == owner->epoch_serial &&
           owner->pending_valid && upload->serial == owner->pending.serial;
}

void jce_instance_upload_discard(JceInstanceUploadOwner *owner, JceInstanceUpload *upload)
{
    if (!upload) return;
    if (matches_pending(owner, upload)) {
        JceTraceScope scope = jce_trace_scope_begin("renderer.instance.discard");
        const JceInstanceUploadIdentity id = upload_identity(upload);
        trace_identity(owner, &id, 0, NULL);
        cancel_pending(owner, 4u);
        trace_state(owner, upload->count, 0, upload->stride, false);
        jce_trace_scope_end(scope, JCE_TRACE_TASK_CANCELLED);
    } else if (jce_trace_enabled()) {
        JceTraceScope scope = jce_trace_scope_begin("renderer.instance.discard");
        const JceInstanceUploadIdentity id = upload_identity(upload);
        trace_identity(owner, &id, 0, NULL);
        jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, "instance.discard_pending",
            0, id.serial, scope.id);
        jce_trace_scope_end(scope, JCE_TRACE_TASK_CANCELLED);
    }
    memset(upload, 0, sizeof *upload);
}

bool jce_instance_upload_commit(JceInstanceUploadOwner *owner, JceInstanceUpload *upload, uint32_t actual)
{
    if (!matches_pending(owner, upload)) {
        trace_rejected(owner, upload ? upload->count : 0u, upload ? upload->stride : 0u,
                         "renderer.instance.upload", JCE_INSTANCE_ERROR_TOKEN, upload, NULL, actual);
        return false;
    }
    JceTraceScope scope = jce_trace_scope_begin("renderer.instance.upload");
    const JceInstanceUpload pending = owner->pending;
    if (actual > pending.count) { owner->last_error = JCE_INSTANCE_ERROR_TOKEN; goto failure; }
    if (!actual) {
        jce_instance_upload_discard(owner, upload);
        jce_trace_scope_end(scope, JCE_TRACE_TASK_CANCELLED);
        return true;
    }
    if (pending.dynamic) {
        JceInstanceStageLease *lease = NULL;
        JceInstanceUploadIdentity id = upload_identity(&pending);
        id.actual = actual;
        bool updated;
        if (jce_trace_enabled() && owner->provider->update_dynamic_traced) {
            /* Track the entire heap allocation retained by the backend, while
             * submitted bytes may be smaller when the fill admitted fewer. */
            lease = jce_instance_upload_trace_prepare(owner, &id, pending.count * pending.stride);
            updated = owner->provider->update_dynamic_traced(owner->user, pending.handle, pending.offset,
                pending.data, actual * pending.stride, lease);
        } else {
            if (jce_trace_enabled()) stage_unavailable(owner, &id, 5u);
            updated = owner->provider->update_dynamic(owner->user, pending.handle, pending.offset,
                                                       pending.data, actual * pending.stride);
        }
        if (!updated) {
            jce_instance_upload_trace_cancel(lease);
            owner->last_error = JCE_INSTANCE_ERROR_UPDATE;
            goto failure;
        }
        /* Backend owns the staging allocation after successful update. */
        owner->pending.data = NULL;
        JceInstanceUploadLane *lane = &owner->lanes[lane_index(pending.stride)];
        if (pending.candidate) {
            if (lane->handle != JCE_INSTANCE_UPLOAD_INVALID) {
                JceInstanceUploadRetired *old = &owner->retired[owner->retired_count++];
                old->handle = lane->handle;
                old->bytes = lane->capacity * lane->stride;
                old->epoch = owner->epoch;
                old->identity = lane->identity;
                trace_resource(owner, &old->identity, "renderer.instance.retire_enqueue", 0u, JCE_TRACE_TASK_SUCCEEDED);
            }
            lane->identity = id;
            lane->handle = pending.handle;
            lane->capacity = pending.capacity;
        }
        lane->cursor = pending.offset + actual;
    }
    owner->last_error = JCE_INSTANCE_ERROR_NONE;
    *upload = pending;
    upload->data = NULL;
    upload->actual = actual;
    upload->committed = true;
    owner->pending_valid = false;
    memset(&owner->pending, 0, sizeof owner->pending);
    trace_state(owner, pending.count, actual, pending.stride, false);
    const JceInstanceUploadIdentity accepted = upload_identity(upload);
    trace_identity(owner, &accepted, actual, NULL);
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.route_dynamic", pending.dynamic ? 1 : 0);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
    return true;
failure:
    {
        const JceInstanceUploadIdentity rejected = upload_identity(&pending);
        trace_identity(owner, &rejected, 0, NULL);
        if (scope.id) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER,
            "instance.attempted_actual", actual, rejected.serial, scope.id);
    }
    warn_partial(owner, pending.count, 0, pending.stride);
    jce_instance_upload_discard(owner, upload);
    trace_state(owner, pending.count, 0, pending.stride, true);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_FAILED);
    return false;
}

bool jce_instance_upload_bind_encoder(const JceInstanceUploadOwner *owner,
    const JceInstanceUpload *upload, uint32_t actual, void *encoder)
{
    if (!owner || !owner->initialized || !upload || !upload->committed ||
        upload->owner != owner || upload->generation != owner->generation ||
        upload->epoch != owner->epoch || upload->epoch_serial != owner->epoch_serial ||
        actual > upload->actual || !actual || lane_index(upload->stride) < 0 ||
        (upload->dynamic && (upload->handle == JCE_INSTANCE_UPLOAD_INVALID ||
          upload->offset > upload->capacity || actual > upload->capacity - upload->offset))) {
        trace_rejected(owner, upload ? upload->count : 0u, upload ? upload->stride : 0u,
                         "renderer.instance.bind", JCE_INSTANCE_ERROR_TOKEN, upload, encoder, actual);
        return false;
    }
    JceTraceScope scope = jce_trace_scope_begin("renderer.instance.bind");
    if (upload->dynamic)
        owner->provider->bind_dynamic(owner->user, encoder, upload->handle, upload->offset, actual);
    else
        owner->provider->bind_transient(owner->user, encoder, &upload->transient, actual);
    trace_state(owner, upload->count, actual, upload->stride, false);
    const JceInstanceUploadIdentity bound = upload_identity(upload);
    trace_identity(owner, &bound, actual, encoder);
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "instance.route_dynamic", upload->dynamic ? 1 : 0);
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
    return true;
}

bool jce_instance_upload_bind(JceInstanceUploadOwner *owner, JceInstanceUpload *upload, uint32_t actual)
{
    if (!upload || !actual) {
        if (upload) jce_instance_upload_discard(owner, upload);
        return actual == 0;
    }
    if (!upload->committed && !jce_instance_upload_commit(owner, upload, actual)) return false;
    return jce_instance_upload_bind_encoder(owner, upload, actual, NULL);
}
