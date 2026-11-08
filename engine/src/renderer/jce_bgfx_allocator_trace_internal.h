/* Opt-in bgfx allocation pairing over the existing bridge and bounded trace.
 * Addresses stay private. Metadata gates never cover provider or trace calls.
 * Begin/end require the existing quiescent init/backend-join boundaries. */
#ifndef JCE_BGFX_ALLOCATOR_TRACE_INTERNAL_H
#define JCE_BGFX_ALLOCATOR_TRACE_INTERNAL_H
#include "os/core/jce_atomic_storage.h"
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

enum { JCE_BGFX_ALLOC_TRACE_LIMIT = 512 }; /* Historical request prefix, no longer a cutoff. */
enum { JCE_BGFX_ALLOC_LEDGER_CAPACITY = 4096 };
enum { JCE_BGFX_ALLOC_OP_ALLOC = 1, JCE_BGFX_ALLOC_OP_REALLOC = 2, JCE_BGFX_ALLOC_OP_FREE = 3 };
enum { JCE_BGFX_ALLOC_SUMMARY_BEGIN = 1, JCE_BGFX_ALLOC_SUMMARY_SNAPSHOT = 2,
       JCE_BGFX_ALLOC_SUMMARY_INIT_FAILED = 3, JCE_BGFX_ALLOC_SUMMARY_JOINED = 4 };
enum { JCE_BGFX_LEDGER_EMPTY = 0, JCE_BGFX_LEDGER_LIVE = 1,
       JCE_BGFX_LEDGER_RESERVED = 2, JCE_BGFX_LEDGER_DELETED = 3 };
typedef void *(*JceBgfxReallocatePort)(void *, size_t, size_t, void *);
typedef struct JceBgfxAllocationEntry {
    uintptr_t address;
    uint64_t serial;
    size_t size;
    uint32_t revision, phase;
} JceBgfxAllocationEntry;
typedef struct JceBgfxAllocatorTrace {
    JceAtomicI32 enabled, admitted, completed, suppressed, failed, zero_frees;
    JceAtomicI32 saturated, capture_gaps, metadata_gate, active_callbacks;
    uint64_t attempt_serial, backend_epoch, next_allocation;
    uint64_t operations, finished, all_failed, all_frees;
    uint64_t live_count, live_bytes, peak_bytes, pending;
    uint64_t overflow, unknown_inputs, duplicates, identity_overflow;
    uint64_t begin_refused, end_refused;
    uint32_t origin_generation;
    bool opted_in, bridge_active, origin_observed, attempt_open;
    bool ledger_bad, events_bad;
    JceBgfxAllocationEntry entries[JCE_BGFX_ALLOC_LEDGER_CAPACITY];
} JceBgfxAllocatorTrace;
typedef struct JceBgfxAllocationSnapshot {
    uint64_t backend_epoch, operations, finished, all_failed, all_frees;
    uint64_t live_count, live_bytes, peak_bytes, pending;
    uint64_t overflow, unknown_inputs, duplicates, identity_overflow;
    uint64_t begin_refused, end_refused;
    bool ledger_bad, events_bad, attempt_open;
} JceBgfxAllocationSnapshot;

static inline int32_t jce_bgfx_trace_increment_bounded(JceAtomicI32 *counter, int32_t limit)
{
    int32_t current = jce_atomic_i32_load(counter);
    while (current < limit) {
        if (jce_atomic_i32_compare_exchange_internal(counter, current, current + 1))
            return current + 1;
        current = jce_atomic_i32_load(counter);
    }
    return 0;
}
/* The gate protects fixed metadata only. No allocation, logging, timing,
 * provider, trace publication or external callback is permitted while held. */
static inline void jce_bgfx_trace_lock(JceBgfxAllocatorTrace *state)
{
    while (!jce_atomic_i32_compare_exchange_internal(&state->metadata_gate, 0, 1)) {}
}
static inline void jce_bgfx_trace_unlock(JceBgfxAllocatorTrace *state)
{ jce_atomic_i32_store(&state->metadata_gate, 0); }
static inline void jce_bgfx_trace_suppress(JceBgfxAllocatorTrace *state)
{
    if (!jce_bgfx_trace_increment_bounded(&state->suppressed, INT_MAX))
        jce_atomic_i32_store(&state->saturated, 1);
}
static inline void jce_bgfx_trace_add(JceBgfxAllocatorTrace *state, uint64_t *value, uint64_t amount)
{
    if (amount > (uint64_t)INT64_MAX || *value > (uint64_t)INT64_MAX - amount) {
        *value = (uint64_t)INT64_MAX;
        state->ledger_bad = true;
        jce_atomic_i32_store(&state->saturated, 1);
    } else *value += amount;
}
static inline uint32_t jce_bgfx_trace_hash(uintptr_t address)
{
    uint64_t key = (uint64_t)address;
    key ^= key >> 33;
    key *= UINT64_C(0xff51afd7ed558ccd);
    key ^= key >> 33;
    return (uint32_t)key & (JCE_BGFX_ALLOC_LEDGER_CAPACITY - 1u);
}
static inline uint32_t jce_bgfx_trace_find(const JceBgfxAllocatorTrace *state, uintptr_t address)
{
    uint32_t index = jce_bgfx_trace_hash(address);
    for (uint32_t n = 0; n < JCE_BGFX_ALLOC_LEDGER_CAPACITY; ++n) {
        const JceBgfxAllocationEntry *entry = &state->entries[index];
        if (entry->phase == JCE_BGFX_LEDGER_EMPTY) break;
        if (entry->phase == JCE_BGFX_LEDGER_LIVE && entry->address == address) return index;
        index = (index + 1u) & (JCE_BGFX_ALLOC_LEDGER_CAPACITY - 1u);
    }
    return JCE_BGFX_ALLOC_LEDGER_CAPACITY;
}
static inline uint32_t jce_bgfx_trace_vacancy(const JceBgfxAllocatorTrace *state, uintptr_t address)
{
    uint32_t index = jce_bgfx_trace_hash(address);
    for (uint32_t n = 0; n < JCE_BGFX_ALLOC_LEDGER_CAPACITY; ++n) {
        const uint32_t phase = state->entries[index].phase;
        if (phase == JCE_BGFX_LEDGER_EMPTY || phase == JCE_BGFX_LEDGER_DELETED) return index;
        index = (index + 1u) & (JCE_BGFX_ALLOC_LEDGER_CAPACITY - 1u);
    }
    return JCE_BGFX_ALLOC_LEDGER_CAPACITY;
}
static inline JceBgfxAllocationSnapshot jce_bgfx_trace_snapshot(JceBgfxAllocatorTrace *state)
{
    JceBgfxAllocationSnapshot out;
    jce_bgfx_trace_lock(state);
#define COPY(name_) out.name_ = state->name_
    COPY(backend_epoch); COPY(operations); COPY(finished); COPY(all_failed); COPY(all_frees);
    COPY(live_count); COPY(live_bytes); COPY(peak_bytes); COPY(pending);
    COPY(overflow); COPY(unknown_inputs); COPY(duplicates); COPY(identity_overflow);
    COPY(begin_refused); COPY(end_refused); COPY(ledger_bad); COPY(events_bad); COPY(attempt_open);
#undef COPY
    jce_bgfx_trace_unlock(state);
    return out;
}
static inline void jce_bgfx_trace_summary(JceBgfxAllocatorTrace *state, uint32_t reason)
{
    if (!state || !state->opted_in || !jce_trace_enabled()) return;
    const JceBgfxAllocationSnapshot snapshot = jce_bgfx_trace_snapshot(state);
    const uint64_t parent = jce_trace_task_current_id();
    const uint32_t generation = jce_trace_capture_generation();
    const bool same_origin = state->origin_observed && generation == state->origin_generation;
    const uint64_t id = same_origin ? state->attempt_serial : 0;
    const bool quiescent = reason != JCE_BGFX_ALLOC_SUMMARY_SNAPSHOT &&
        snapshot.pending == 0 && jce_atomic_i32_load(&state->active_callbacks) == 0;
    const bool ledger_complete = state->bridge_active && !snapshot.ledger_bad;
#define BS(name_, value_) jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, \
    "bgfx.alloc." name_, (int64_t)(value_), id, parent)
    BS("attempt_serial", state->attempt_serial); BS("backend_epoch", snapshot.backend_epoch);
    BS("summary_reason", reason); BS("bridge_active", state->bridge_active ? 1 : 0);
    BS("request_limit", 0); /* Existing bounded ring, no lifetime request truncation. */
    BS("admitted_requests", jce_atomic_i32_load(&state->admitted));
    BS("completed_requests", jce_atomic_i32_load(&state->completed));
    BS("suppressed_requests", jce_atomic_i32_load(&state->suppressed));
    BS("failed_requests", jce_atomic_i32_load(&state->failed));
    BS("zero_size_frees", jce_atomic_i32_load(&state->zero_frees));
    BS("capture_gaps", jce_atomic_i32_load(&state->capture_gaps));
    BS("counts_saturated", jce_atomic_i32_load(&state->saturated));
    BS("attempt_capture_generation", state->origin_generation);
    BS("current_capture_generation", generation); BS("origin_capture_gap", !same_origin);
    BS("summary_quiescent", quiescent); BS("attempt_open", snapshot.attempt_open);
    BS("ledger_capacity", JCE_BGFX_ALLOC_LEDGER_CAPACITY);
    BS("ledger_storage_bytes", sizeof(state->entries));
    BS("operations_total", snapshot.operations); BS("operations_finished", snapshot.finished);
    BS("all_failed_requests", snapshot.all_failed); BS("all_zero_size_frees", snapshot.all_frees);
    BS("tracked_live_count", snapshot.live_count); BS("tracked_live_bytes", snapshot.live_bytes);
    /* Accounting peak includes reserved in-flight old records, not physical residency. */
    BS("tracked_peak_bytes", snapshot.peak_bytes); BS("inflight_operations", snapshot.pending);
    BS("ledger_overflow", snapshot.overflow); BS("unknown_inputs", snapshot.unknown_inputs);
    BS("duplicate_addresses", snapshot.duplicates); BS("identity_overflow", snapshot.identity_overflow);
    BS("begin_refused", snapshot.begin_refused); BS("end_refused", snapshot.end_refused);
    BS("ledger_complete", ledger_complete);
    /* Global publication loss is checked separately by the existing capture result. */
    BS("event_history_complete", ledger_complete && same_origin && !snapshot.events_bad);
    BS("all_released", ledger_complete && quiescent && !snapshot.attempt_open && snapshot.live_count == 0);
#undef BS
}
static inline void jce_bgfx_trace_begin(JceBgfxAllocatorTrace *state, bool opt_in, bool bridge_active)
{
    if (!state) return;
    /* Existing renderer admission makes begin quiescent. Defend against a
     * misplaced private call without clearing live or reserved old records. */
    if (jce_atomic_i32_load(&state->active_callbacks) != 0 || state->attempt_open) {
        jce_bgfx_trace_lock(state);
        state->ledger_bad = state->events_bad = true;
        jce_bgfx_trace_add(state, &state->begin_refused, 1);
        jce_bgfx_trace_unlock(state);
        return;
    }
    jce_atomic_i32_store(&state->enabled, 0);
    state->opted_in = opt_in; state->bridge_active = bridge_active;
    if (!opt_in) return; /* No ledger clearing, timing or work on the default path. */
    (void)jce_time_ticks_ns(); /* Warm the OS timer before allocator callbacks. */
    const uint64_t previous_epoch = state->backend_epoch;
    memset(state->entries, 0, sizeof(state->entries));
    state->next_allocation = state->operations = state->finished = 0;
    state->all_failed = state->all_frees = state->live_count = state->live_bytes = state->peak_bytes = 0;
    state->pending = state->overflow = state->unknown_inputs = state->duplicates = 0;
    state->identity_overflow = state->begin_refused = state->end_refused = 0;
    state->ledger_bad = state->events_bad = false;
    state->backend_epoch = previous_epoch < (uint64_t)INT64_MAX ? previous_epoch + 1 : previous_epoch;
    if (previous_epoch == (uint64_t)INT64_MAX) {
        /* Never publish a recycled epoch/serial tuple after exhaustion. */
        state->ledger_bad = state->events_bad = true;
        state->next_allocation = (uint64_t)INT64_MAX;
        state->identity_overflow = 1;
    }
    jce_atomic_i32_store(&state->admitted, 0); jce_atomic_i32_store(&state->completed, 0);
    jce_atomic_i32_store(&state->suppressed, 0); jce_atomic_i32_store(&state->failed, 0);
    jce_atomic_i32_store(&state->zero_frees, 0); jce_atomic_i32_store(&state->saturated, 0);
    jce_atomic_i32_store(&state->capture_gaps, 0);
    if (previous_epoch == (uint64_t)INT64_MAX) jce_atomic_i32_store(&state->saturated, 1);
    state->attempt_serial = jce_trace_next_id();
    state->origin_generation = jce_trace_capture_generation();
    state->origin_observed = jce_trace_enabled();
    state->attempt_open = true;
    jce_atomic_i32_store(&state->enabled, bridge_active ? 1 : 0);
    jce_bgfx_trace_summary(state, JCE_BGFX_ALLOC_SUMMARY_BEGIN);
}
/* Only after bgfx_init failed cleanup or the real bgfx_shutdown join. Does
 * not erase a residual allocation: the terminal counters retain its bytes. */
static inline void jce_bgfx_trace_end(JceBgfxAllocatorTrace *state, uint32_t reason)
{
    if (!state || !state->opted_in) return;
    if (jce_atomic_i32_load(&state->active_callbacks) != 0) {
        jce_bgfx_trace_lock(state);
        state->ledger_bad = state->events_bad = true;
        jce_bgfx_trace_add(state, &state->end_refused, 1);
        jce_bgfx_trace_unlock(state);
        jce_bgfx_trace_summary(state, reason);
        return;
    }
    jce_atomic_i32_store(&state->enabled, 0);
    jce_bgfx_trace_lock(state);
    state->attempt_open = false;
    jce_bgfx_trace_unlock(state);
    jce_bgfx_trace_summary(state, reason);
}
static inline void *jce_bgfx_trace_reallocate(JceBgfxAllocatorTrace *state,
    JceBgfxReallocatePort reallocate, void *user, void *ptr, size_t size, size_t alignment, uint32_t line)
{
    if (!state || !jce_atomic_i32_load(&state->enabled))
        return reallocate(ptr, size, alignment, user);
    jce_atomic_i32_add(&state->active_callbacks, 1);
    const bool emitting = jce_trace_enabled();
    const int32_t index = emitting ? jce_bgfx_trace_increment_bounded(&state->admitted, INT_MAX) : 0;
    const uint64_t id = index ? jce_trace_next_id() : 0;
    const uint64_t parent = index ? jce_trace_task_current_id() : 0;
    const uint32_t generation = jce_trace_capture_generation();
    const uint32_t operation = !size ? JCE_BGFX_ALLOC_OP_FREE :
        ptr ? JCE_BGFX_ALLOC_OP_REALLOC : JCE_BGFX_ALLOC_OP_ALLOC;
    JceBgfxAllocationEntry old;
    memset(&old, 0, sizeof(old));
    uint32_t reserved = JCE_BGFX_ALLOC_LEDGER_CAPACITY;
    jce_bgfx_trace_lock(state);
    jce_bgfx_trace_add(state, &state->operations, 1);
    ++state->pending;
    if (!index) {
        state->events_bad = true;
        jce_bgfx_trace_suppress(state);
        if (emitting) jce_atomic_i32_store(&state->saturated, 1);
    }
    if (!state->origin_observed || generation != state->origin_generation) state->events_bad = true;
    if (ptr) {
        reserved = jce_bgfx_trace_find(state, (uintptr_t)ptr);
        if (reserved != JCE_BGFX_ALLOC_LEDGER_CAPACITY) {
            old = state->entries[reserved];
            state->entries[reserved].phase = JCE_BGFX_LEDGER_RESERVED;
        } else {
            jce_bgfx_trace_add(state, &state->unknown_inputs, 1);
            state->ledger_bad = true;
        }
    }
    jce_bgfx_trace_unlock(state);
#define BR(name_, value_) jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, \
    "bgfx.alloc." name_, (int64_t)(value_), id, parent)
    if (index) {
        BR("attempt_serial", state->attempt_serial); BR("backend_epoch", state->backend_epoch);
        BR("request_index", index); BR("request_capture_generation", generation); BR("operation", operation);
        BR("input_nonnull", ptr ? 1 : 0);
        BR("requested_size", (uint64_t)size > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)size);
        BR("size_saturated", (uint64_t)size > (uint64_t)INT64_MAX);
        BR("alignment", (uint64_t)alignment > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)alignment);
        BR("alignment_saturated", (uint64_t)alignment > (uint64_t)INT64_MAX);
        BR("source_line", line);
    }
    /* The immutable production port is raw mimalloc. The ring-only writer
     * and fixed metadata do not allocate or call a profiler: no self-trace. */
    void *result = reallocate(ptr, size, alignment, user);
    const bool succeeded = !size ? result == NULL : result != NULL;
    const uint32_t after_generation = jce_trace_capture_generation();
    const bool result_observed = index && after_generation == generation && jce_trace_enabled();
    uint64_t allocation_id = old.serial;
    uint64_t new_size = old.serial ? (uint64_t)old.size : 0;
    uint32_t revision = old.revision;
    bool paired = false;
    jce_bgfx_trace_lock(state);
    jce_bgfx_trace_add(state, &state->finished, 1);
    if (!succeeded) jce_bgfx_trace_add(state, &state->all_failed, 1);
    if (!size) jce_bgfx_trace_add(state, &state->all_frees, 1);
    if (index) {
        if (!jce_bgfx_trace_increment_bounded(&state->completed, INT_MAX))
            jce_atomic_i32_store(&state->saturated, 1);
        if (!succeeded && !jce_bgfx_trace_increment_bounded(&state->failed, INT_MAX))
            jce_atomic_i32_store(&state->saturated, 1);
        if (!size && !jce_bgfx_trace_increment_bounded(&state->zero_frees, INT_MAX))
            jce_atomic_i32_store(&state->saturated, 1);
        if (!result_observed) {
            state->events_bad = true;
            if (!jce_bgfx_trace_increment_bounded(&state->capture_gaps, INT_MAX))
                jce_atomic_i32_store(&state->saturated, 1);
        }
    }
    if (old.serial && !succeeded) {
        state->entries[reserved] = old;
        paired = true; /* Failed realloc preserves the old identity/bytes. */
        if (!size) state->ledger_bad = true; /* Broken free-port contract. */
    } else if (old.serial && succeeded && !size) {
        state->entries[reserved].address = 0;
        state->entries[reserved].phase = JCE_BGFX_LEDGER_DELETED;
        if (!state->live_count || state->live_bytes < (uint64_t)old.size) state->ledger_bad = true;
        state->live_count = state->live_count ? state->live_count - 1 : 0;
        state->live_bytes = state->live_bytes >= old.size ? state->live_bytes - old.size : 0;
        new_size = 0; paired = true;
    } else if (succeeded && size) {
        const uintptr_t address = (uintptr_t)result;
        if (old.serial) {
            state->entries[reserved].address = 0;
            state->entries[reserved].phase = JCE_BGFX_LEDGER_DELETED;
            if (!state->live_count || state->live_bytes < (uint64_t)old.size) state->ledger_bad = true;
            state->live_count = state->live_count ? state->live_count - 1 : 0;
            state->live_bytes = state->live_bytes >= old.size ? state->live_bytes - old.size : 0;
            if (revision != UINT32_MAX) ++revision;
            else { state->ledger_bad = true; jce_atomic_i32_store(&state->saturated, 1); }
        } else if (state->next_allocation < (uint64_t)INT64_MAX) {
            allocation_id = ++state->next_allocation;
        } else {
            allocation_id = 0;
            state->ledger_bad = true;
            jce_bgfx_trace_add(state, &state->identity_overflow, 1);
        }
        const bool duplicate = jce_bgfx_trace_find(state, address) != JCE_BGFX_ALLOC_LEDGER_CAPACITY;
        const uint32_t slot = duplicate ? JCE_BGFX_ALLOC_LEDGER_CAPACITY : jce_bgfx_trace_vacancy(state, address);
        if (duplicate) {
            state->ledger_bad = true;
            jce_bgfx_trace_add(state, &state->duplicates, 1);
        } else if (slot == JCE_BGFX_ALLOC_LEDGER_CAPACITY || !allocation_id) {
            state->ledger_bad = true;
            jce_bgfx_trace_add(state, &state->overflow, 1);
        } else {
            JceBgfxAllocationEntry *entry = &state->entries[slot];
            entry->address = address; entry->serial = allocation_id;
            entry->size = size; entry->revision = revision; entry->phase = JCE_BGFX_LEDGER_LIVE;
            jce_bgfx_trace_add(state, &state->live_count, 1);
            jce_bgfx_trace_add(state, &state->live_bytes, (uint64_t)size);
            if (state->live_bytes > state->peak_bytes) state->peak_bytes = state->live_bytes;
            paired = true;
        }
        new_size = (uint64_t)size;
    } else if (!ptr && !size && succeeded) paired = true; /* Explicit no-op free. */
    --state->pending;
    jce_bgfx_trace_unlock(state);
    if (result_observed) {
        BR("returned_nonnull", result ? 1 : 0); BR("operation_succeeded", succeeded ? 1 : 0);
        BR("allocation_id", allocation_id); BR("input_known", old.serial ? 1 : 0);
        BR("old_size", (uint64_t)old.size > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)old.size);
        BR("new_size", new_size > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)new_size);
        BR("allocation_revision", revision); BR("paired", paired ? 1 : 0);
        BR("lineage_known", !ptr || old.serial ? 1 : 0);
    } else if (index && jce_trace_enabled()) {
        const uint64_t gap = jce_trace_next_id(), current_parent = jce_trace_task_current_id();
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, "bgfx.alloc.result_capture_gap", 1, gap, current_parent);
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, "bgfx.alloc.origin_request_id", (int64_t)id, gap, current_parent);
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, "bgfx.alloc.origin_request_generation", generation, gap, current_parent);
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, "bgfx.alloc.current_capture_generation", after_generation, gap, current_parent);
    }
#undef BR
    jce_atomic_i32_add(&state->active_callbacks, -1);
    return result;
}
#endif
