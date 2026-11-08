/*
 * jce_trace.c - Bounded multi-producer runtime event timeline.
 */

#include <jce/os/core/jce_trace.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_assert.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>

#include "jce_memory.h"
#include "jce_trace_capture_internal.h"
#include "jce_trace_scope_internal.h"
#include "jce_trace_clock_internal.h"
#include "os/platform/jce_native_atomic_word_internal.h"

#include <SDL3/SDL.h>

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Fixed power-of-two backlog allowance; publication still reports overwrites.
 * The continuous consumer's bounded final drain covers this entire ring. */
#define TRACE_CAPACITY 65536u
#define TRACE_MASK     (TRACE_CAPACITY - 1u)

typedef struct TraceSlot {
    SDL_SpinLock lock;
    SDL_AtomicU32 published;
    JceTraceEvent event;
} TraceSlot;

typedef struct TraceU64 {
    SDL_AtomicInt version;
    SDL_AtomicInt lo;
    SDL_AtomicInt hi;
} TraceU64;

/* Capture identity has one process owner. Its selected ID strategy and
 * registration accounting epoch share the existing quiescent reset boundary. */
typedef struct TraceIdentityState {
#if UINTPTR_MAX > UINT32_MAX
    JceNativeAtomicWord next_id;
#else
    SDL_SpinLock next_id_lock;
    uint64_t next_id;
#endif
    /* Enable changes capture generation; only quiescent reset advances this
     * single-writer epoch and invalidates an older thread's accounting. */
    TraceU64 registration_epoch;
} TraceIdentityState;

static TraceSlot s_slots[TRACE_CAPACITY];
static SDL_AtomicInt s_enabled;
/* UINT32_MAX is permanent exhaustion, not a publishable event sequence. */
static SDL_AtomicU32 s_write_sequence;
static TraceIdentityState s_identity;
static SDL_AtomicInt s_generation;
static SDL_AtomicInt s_active_threads;
static SDL_AtomicInt s_active_work_items;
static SDL_AtomicInt s_active_waits;
static SDL_AtomicInt s_tasks_submitted;
static SDL_AtomicInt s_work_items_started;
static SDL_AtomicInt s_work_items_completed;
static SDL_AtomicInt s_waits_completed;
static SDL_AtomicInt s_max_queue_ns;
static SDL_AtomicInt s_max_run_ns;
static SDL_AtomicInt s_max_wait_ns;
static SDL_AtomicInt s_max_queue_us;
static SDL_AtomicInt s_max_run_us;
static SDL_AtomicInt s_max_wait_us;
static SDL_AtomicInt s_max_queue_ms;
static SDL_AtomicInt s_max_run_ms;
static SDL_AtomicInt s_max_wait_ms;
static TraceU64 s_last_wall_ns;
static TraceU64 s_last_sim_ns;
static TraceU64 s_last_frame_index;
/* Same compiler-TLS mechanism as the profiler, with one trace context owner.
 * Full-width IDs must never pass through a 32-bit pointer-sized SDL sentinel. */
#if JCE_COMPILER_MSVC
#define JCE_TRACE_TLS __declspec(thread)
#elif JCE_COMPILER_GCC || JCE_COMPILER_CLANG
#define JCE_TRACE_TLS __thread
#else
#error "Trace context needs a supported compiler TLS implementation"
#endif
typedef struct TraceThreadState {
    uint64_t task;
    uintptr_t suppression;
    uint32_t generation;
    uint64_t registration_epoch;
    bool registered;
    bool active;
} TraceThreadState;
static JCE_TRACE_TLS TraceThreadState s_thread;

static void trace_id_store_quiescent(uint64_t value)
{
#if UINTPTR_MAX > UINT32_MAX
    jce_native_atomic_word_store(&s_identity.next_id, (size_t)value);
#else
    /* Reset/seed is quiescent, just like the ring reset contract. */
    s_identity.next_id = value;
#endif
}

static bool trace_id_reserve(uint64_t *id)
{
    uint64_t next;
#if UINTPTR_MAX > UINT32_MAX
    size_t current = jce_native_atomic_word_load(&s_identity.next_id);
    for (;;) {
        if (current == UINT64_MAX) return false;
        next = (uint64_t)current + 1u;
        if (jce_native_atomic_word_compare_exchange(&s_identity.next_id, current,
                                                     (size_t)next)) break;
        current = jce_native_atomic_word_load(&s_identity.next_id);
    }
#else
    SDL_LockSpinlock(&s_identity.next_id_lock);
    if (s_identity.next_id == UINT64_MAX) {
        SDL_UnlockSpinlock(&s_identity.next_id_lock);
        return false;
    }
    next = ++s_identity.next_id;
    SDL_UnlockSpinlock(&s_identity.next_id_lock);
#endif
    *id = next;
    return true;
}

static bool trace_sequence_reserve(uint32_t *sequence)
{
    uint32_t current = SDL_GetAtomicU32(&s_write_sequence);
    for (;;) {
        if (current == UINT32_MAX) return false;
        const uint32_t next = current + 1u;
        if (SDL_CompareAndSwapAtomicU32(&s_write_sequence, current, next)) {
            if (next == UINT32_MAX) {
                /* Preserve this invalid state across disable/re-enable. */
                SDL_SetAtomicInt(&s_enabled, 0);
                return false;
            }
            *sequence = next;
            return true;
        }
        current = SDL_GetAtomicU32(&s_write_sequence);
    }
}

bool jce_trace_capture_valid_internal(void)
{
    return SDL_GetAtomicU32(&s_write_sequence) != UINT32_MAX;
}

static uint64_t trace_u64_load(const TraceU64 *value)
{
    int before = 0;
    int after = 0;
    uint32_t lo = 0;
    uint32_t hi = 0;

    do {
        before = SDL_GetAtomicInt((SDL_AtomicInt *)&value->version);
        if ((before & 1) != 0)
            continue;
        SDL_MemoryBarrierAcquire();
        lo = (uint32_t)SDL_GetAtomicInt((SDL_AtomicInt *)&value->lo);
        hi = (uint32_t)SDL_GetAtomicInt((SDL_AtomicInt *)&value->hi);
        SDL_MemoryBarrierAcquire();
        after = SDL_GetAtomicInt((SDL_AtomicInt *)&value->version);
    } while (before != after || (after & 1) != 0);

    return ((uint64_t)hi << 32u) | (uint64_t)lo;
}

static void trace_u64_store(TraceU64 *value, uint64_t input)
{
    SDL_AddAtomicInt(&value->version, 1);
    SDL_MemoryBarrierRelease();
    SDL_SetAtomicInt(&value->lo, (int)(uint32_t)input);
    SDL_SetAtomicInt(&value->hi, (int)(uint32_t)(input >> 32u));
    SDL_MemoryBarrierRelease();
    SDL_AddAtomicInt(&value->version, 1);
}

static void trace_atomic_max(SDL_AtomicInt *target, uint64_t value)
{
    int desired = value > (uint64_t)INT_MAX ? INT_MAX : (int)value;
    int current = SDL_GetAtomicInt(target);

    while (desired > current &&
           !SDL_CompareAndSwapAtomicInt(target, current, desired))
        current = SDL_GetAtomicInt(target);
}

static void trace_duration_max(SDL_AtomicInt *nanoseconds,
                               SDL_AtomicInt *microseconds,
                               SDL_AtomicInt *milliseconds,
                               uint64_t value_ns)
{
    trace_atomic_max(nanoseconds, value_ns);
    trace_atomic_max(microseconds, value_ns / UINT64_C(1000));
    trace_atomic_max(milliseconds, value_ns / UINT64_C(1000000));
}

static uint64_t trace_duration_load(SDL_AtomicInt *nanoseconds,
                                    SDL_AtomicInt *microseconds,
                                    SDL_AtomicInt *milliseconds)
{
    uint64_t exact_ns =
        (uint32_t)SDL_GetAtomicInt(nanoseconds);
    uint64_t coarse_us =
        (uint64_t)(uint32_t)SDL_GetAtomicInt(microseconds) * UINT64_C(1000);
    uint64_t coarse_ms =
        (uint64_t)(uint32_t)SDL_GetAtomicInt(milliseconds) * UINT64_C(1000000);
    uint64_t result = exact_ns > coarse_us ? exact_ns : coarse_us;

    return result > coarse_ms ? result : coarse_ms;
}

static bool trace_thread_suppressed(void)
{
    return s_thread.suppression != 0;
}

bool jce_trace_suppress_begin(void)
{
    uintptr_t depth = s_thread.suppression;
    if (depth == UINTPTR_MAX) return false;
    s_thread.suppression = depth + 1;
    return true;
}

bool jce_trace_suppress_end(void)
{
    uintptr_t depth = s_thread.suppression;
    if (!depth) return false;
    s_thread.suppression = depth - 1;
    return true;
}

static uint64_t trace_current_task_id(void)
{
    return s_thread.task;
}

static void trace_set_current_task_id(uint64_t id)
{
    s_thread.task = id;
}

static void trace_copy_name(char dst[JCE_TRACE_NAME_CAP], const char *name)
{
    SDL_strlcpy(dst, name && name[0] ? name : "unnamed",
                JCE_TRACE_NAME_CAP);
}

/* Delayed publishers must not replace a newer wrap of the same slot.
 * Their reserved sequence remains a real missing/overwritten event. */
static void trace_publish_slot(TraceSlot *slot, const JceTraceEvent *event)
{
    SDL_LockSpinlock(&slot->lock);
    if (SDL_GetAtomicU32(&slot->published) <= event->sequence) {
        slot->event = *event;
        SDL_SetAtomicU32(&slot->published, event->sequence);
    }
    SDL_UnlockSpinlock(&slot->lock);
}

static void trace_emit(JceTraceEventType type,
                       JceTraceCategory category,
                       uint64_t id,
                       uint64_t parent_id,
                       uint64_t value_u64,
                       uint32_t value0,
                       uint32_t value1,
                       const char *name)
{
    JceTraceEvent event;
    TraceSlot *slot;
    uint32_t sequence;

    if (!jce_trace_enabled())
        return;

    SDL_zero(event);
    if (!trace_sequence_reserve(&sequence)) return;

    event.sequence = sequence;
    event.type = type;
    event.category = category;
    /* Reserve before reading: an invalid/overflowing clock leaves an explicit
     * pending sequence, so full-stream accounting refuses incomplete evidence.
     * Never fabricate zero/saturated time or reset away a previous session. */
    if (!jce_trace_clock_now_internal(&event.timestamp_ns)) return;
    event.thread_id = jce_thread_current_id();
    event.id = id;
    event.parent_id = parent_id;
    event.value_u64 = value_u64;
    event.value_u32[0] = value0;
    event.value_u32[1] = value1;
    trace_copy_name(event.name, name);

    slot = &s_slots[(sequence - 1u) & TRACE_MASK];
    trace_publish_slot(slot, &event);
}

void jce_trace_set_enabled(bool enabled)
{
    int was_enabled = SDL_GetAtomicInt(&s_enabled);

    enabled = enabled && jce_trace_capture_valid_internal();
    if (enabled && was_enabled == 0)
        SDL_AddAtomicInt(&s_generation, 1);
    SDL_SetAtomicInt(&s_enabled, enabled ? 1 : 0);
    /* A producer can exhaust the sequence after the first validity read but
     * before this store. Keep the repair on this cold configuration path. */
    if (enabled && !jce_trace_capture_valid_internal())
        SDL_SetAtomicInt(&s_enabled, 0);
}

bool jce_trace_enabled(void)
{
    return SDL_GetAtomicInt(&s_enabled) != 0 && !trace_thread_suppressed();
}

void jce_trace_reset(void)
{
    uint32_t i;

    /* Public reset requires all producers/consumers to be quiescent. */
    for (i = 0; i < TRACE_CAPACITY; ++i) {
        s_slots[i].lock = 0;
        SDL_SetAtomicU32(&s_slots[i].published, 0);
    }
    SDL_SetAtomicU32(&s_write_sequence, 0);
    trace_id_store_quiescent(0);
    SDL_SetAtomicInt(&s_active_threads, 0);
    SDL_SetAtomicInt(&s_active_work_items, 0);
    SDL_SetAtomicInt(&s_active_waits, 0);
    SDL_SetAtomicInt(&s_tasks_submitted, 0);
    SDL_SetAtomicInt(&s_work_items_started, 0);
    SDL_SetAtomicInt(&s_work_items_completed, 0);
    SDL_SetAtomicInt(&s_waits_completed, 0);
    SDL_SetAtomicInt(&s_max_queue_ns, 0);
    SDL_SetAtomicInt(&s_max_run_ns, 0);
    SDL_SetAtomicInt(&s_max_wait_ns, 0);
    SDL_SetAtomicInt(&s_max_queue_us, 0);
    SDL_SetAtomicInt(&s_max_run_us, 0);
    SDL_SetAtomicInt(&s_max_wait_us, 0);
    SDL_SetAtomicInt(&s_max_queue_ms, 0);
    SDL_SetAtomicInt(&s_max_run_ms, 0);
    SDL_SetAtomicInt(&s_max_wait_ms, 0);
    trace_u64_store(&s_last_wall_ns, 0);
    trace_u64_store(&s_last_sim_ns, 0);
    trace_u64_store(&s_last_frame_index, 0);
    trace_u64_store(&s_identity.registration_epoch,
                    trace_u64_load(&s_identity.registration_epoch) + 1u);
    SDL_AddAtomicInt(&s_generation, 1);
    trace_set_current_task_id(0);
}

uint64_t jce_trace_next_id(void)
{
    uint64_t id;
    if (trace_id_reserve(&id)) return id;
    /* No nonzero unique value remains. Even an assertion handler that returns
     * must not allow a duplicate/zero ID to escape this public contract. */
    jce_assert_fail(__FILE__, __LINE__, __func__, "trace ID space available", NULL);
    abort();
}

uint64_t jce_trace_task_current_id(void)
{
    return trace_current_task_id();
}

void jce_trace_task_restore_id(uint64_t task_id)
{
    trace_set_current_task_id(task_id);
}

void jce_trace_thread_register(const char *name)
{
    if (trace_thread_suppressed()) return;
    uint32_t generation = (uint32_t)SDL_GetAtomicInt(&s_generation);
    uint64_t registration_epoch = trace_u64_load(&s_identity.registration_epoch);

    if (s_thread.active && s_thread.registration_epoch != registration_epoch)
        s_thread.active = false; /* Reset already cleared this old accounting. */
    if (s_thread.registered && s_thread.generation == generation &&
        s_thread.registration_epoch == registration_epoch)
        return;
    s_thread.generation = generation;
    s_thread.registration_epoch = registration_epoch;
    s_thread.registered = true;
    jce_log_set_thread_name(name && name[0] ? name : "jce-thread");
    JCE_PROFILE_THREAD_NAME(name && name[0] ? name : "jce-thread");

    if (s_thread.active || !jce_trace_enabled())
        return;
    s_thread.active = true;
    SDL_AddAtomicInt(&s_active_threads, 1);
    trace_emit(JCE_TRACE_EVENT_THREAD_REGISTER,
               JCE_TRACE_CATEGORY_THREAD,
               jce_thread_current_id(), 0, 0, 0, 0, name);
}

void jce_trace_thread_unregister(void)
{
    if (trace_thread_suppressed()) return;
    uint64_t registration_epoch = trace_u64_load(&s_identity.registration_epoch);

    if (!s_thread.registered)
        return;
    if (s_thread.active && s_thread.registration_epoch == registration_epoch) {
        trace_emit(JCE_TRACE_EVENT_THREAD_UNREGISTER,
                   JCE_TRACE_CATEGORY_THREAD,
                   jce_thread_current_id(), 0, 0, 0, 0, "thread-exit");
        SDL_AddAtomicInt(&s_active_threads, -1);
    }
    s_thread.registered = false;
    s_thread.active = false;
}

void jce_trace_task_submit(uint64_t task_id, uint64_t parent_id,
                           JceTraceTaskKind kind, const char *name)
{
    if (!jce_trace_enabled())
        return;
    if (task_id == 0)
        return;
    if (parent_id == 0)
        parent_id = trace_current_task_id();
    SDL_AddAtomicInt(&s_tasks_submitted, 1);
    trace_emit(JCE_TRACE_EVENT_TASK_SUBMIT, JCE_TRACE_CATEGORY_TASK,
               task_id, parent_id, 0, (uint32_t)kind, 0, name);
}

void jce_trace_task_begin(uint64_t span_id, uint64_t task_id,
                          const char *name, uint64_t queue_time_ns)
{
    if (!jce_trace_enabled())
        return;
    if (span_id == 0 || task_id == 0)
        return;
    SDL_AddAtomicInt(&s_active_work_items, 1);
    SDL_AddAtomicInt(&s_work_items_started, 1);
    trace_duration_max(&s_max_queue_ns, &s_max_queue_us, &s_max_queue_ms,
                       queue_time_ns);
    trace_set_current_task_id(task_id);
    trace_emit(JCE_TRACE_EVENT_TASK_BEGIN, JCE_TRACE_CATEGORY_TASK,
               span_id, task_id, queue_time_ns, 0, 0, name);
}

void jce_trace_task_end(uint64_t span_id, uint64_t task_id,
                        const char *name, JceTraceTaskState state,
                        uint64_t run_time_ns)
{
    if (!jce_trace_enabled())
        return;
    if (span_id == 0 || task_id == 0)
        return;
    trace_duration_max(&s_max_run_ns, &s_max_run_us, &s_max_run_ms,
                       run_time_ns);
    trace_emit(JCE_TRACE_EVENT_TASK_END, JCE_TRACE_CATEGORY_TASK,
               span_id, task_id, run_time_ns, (uint32_t)state, 0, name);
    trace_set_current_task_id(0);
    SDL_AddAtomicInt(&s_active_work_items, -1);
    SDL_AddAtomicInt(&s_work_items_completed, 1);
}

void jce_trace_wait_begin(uint64_t wait_id, uint64_t task_id,
                          const char *name)
{
    if (!jce_trace_enabled())
        return;
    if (wait_id == 0)
        return;
    if (task_id == 0)
        task_id = trace_current_task_id();
    SDL_AddAtomicInt(&s_active_waits, 1);
    trace_emit(JCE_TRACE_EVENT_WAIT_BEGIN, JCE_TRACE_CATEGORY_WAIT,
               wait_id, task_id, 0, 0, 0, name);
}

void jce_trace_wait_end(uint64_t wait_id, uint64_t task_id,
                        const char *name, uint64_t wait_time_ns)
{
    if (!jce_trace_enabled())
        return;
    if (wait_id == 0)
        return;
    trace_duration_max(&s_max_wait_ns, &s_max_wait_us, &s_max_wait_ms,
                       wait_time_ns);
    trace_emit(JCE_TRACE_EVENT_WAIT_END, JCE_TRACE_CATEGORY_WAIT,
               wait_id, task_id, wait_time_ns, 0, 0, name);
    SDL_AddAtomicInt(&s_active_waits, -1);
    SDL_AddAtomicInt(&s_waits_completed, 1);
}

void jce_trace_frame_mark(uint64_t frame_index, uint64_t wall_time_ns,
                          uint64_t simulation_time_ns)
{
    if (!jce_trace_enabled())
        return;
    trace_u64_store(&s_last_frame_index, frame_index);
    trace_u64_store(&s_last_wall_ns, wall_time_ns);
    trace_u64_store(&s_last_sim_ns, simulation_time_ns);
    JCE_PROFILE_PLOT("frame.wall_ms", (double)wall_time_ns / 1000000.0);
    JCE_PROFILE_PLOT("frame.sim_ms", (double)simulation_time_ns / 1000000.0);
    trace_emit(JCE_TRACE_EVENT_FRAME, JCE_TRACE_CATEGORY_FRAME,
               frame_index, 0, wall_time_ns,
               simulation_time_ns > UINT32_MAX
                   ? UINT32_MAX : (uint32_t)simulation_time_ns,
               0, "frame");
}

void jce_trace_counter_scoped_internal(JceTraceCategory category, const char *name,
                                        int64_t value, uint64_t id, uint64_t parent)
{
    if (SDL_GetAtomicInt(&s_enabled) != 0 && trace_thread_suppressed()) return;
    JCE_PROFILE_PLOT_I(name && name[0] ? name : "trace.counter", value);
    trace_emit(JCE_TRACE_EVENT_COUNTER, category, id, parent,
               (uint64_t)value, 0, 0, name);
}

void jce_trace_counter_ring_internal(JceTraceCategory category, const char *name,
                                     int64_t value, uint64_t id, uint64_t parent)
{
    trace_emit(JCE_TRACE_EVENT_COUNTER, category, id, parent,
               (uint64_t)value, 0, 0, name);
}

void jce_trace_counter(JceTraceCategory category, const char *name,
                       int64_t value)
{
    const uint64_t span = jce_trace_enabled() ? trace_current_task_id() : 0;
    jce_trace_counter_scoped_internal(category, name, value, span, span);
}

void jce_trace_cursor_init(JceTraceCursor *cursor)
{
    if (!cursor)
        return;
    cursor->next_sequence = 0;
    cursor->lost_events = 0;
}

static uint32_t trace_read_impl(JceTraceCursor *cursor, JceTraceEvent *out_events,
                               uint32_t max_events, JceTraceReadObservation *observation)
{
    uint32_t end = 0, earliest = 0;
    uint32_t sequence = cursor ? cursor->next_sequence : 0;
    uint32_t count = 0, blocked = 0, published_at_stop = 0;
    const uint32_t before = sequence;
    const uint64_t lost_before = cursor ? cursor->lost_events : 0;
    JceTraceReadStop stop = JCE_TRACE_READ_INVALID_ARGS;
    bool published_valid = false;

    if (!cursor || !out_events || max_events == 0)
        goto finished;
    stop = JCE_TRACE_READ_CAUGHT_UP;
    end = SDL_GetAtomicU32(&s_write_sequence);
    if (end == UINT32_MAX) --end; /* The exhaustion sentinel has no event. */
    if (end == 0)
        goto finished;
    earliest = end >= TRACE_CAPACITY ? end - TRACE_CAPACITY + 1u : 1u;
    if (sequence == 0)
        sequence = earliest;
    else if (sequence < earliest) {
        cursor->lost_events += (uint64_t)(earliest - sequence);
        sequence = earliest;
    }

    while (sequence <= end && count < max_events) {
        TraceSlot *slot = &s_slots[(sequence - 1u) & TRACE_MASK];
        /* A producer can reserve end before publishing its slot. Keep the
         * cursor at busy/pending slots; never race a non-atomic event copy. */
        if (!SDL_TryLockSpinlock(&slot->lock)) {
            stop = JCE_TRACE_READ_SLOT_BUSY; blocked = sequence;
            break;
        }
        uint32_t published = SDL_GetAtomicU32(&slot->published);
        if (published == sequence) {
            out_events[count++] = slot->event;
        } else if (published < sequence) {
            SDL_UnlockSpinlock(&slot->lock);
            stop = JCE_TRACE_READ_RESERVED_PENDING; blocked = sequence;
            published_at_stop = published; published_valid = true;
            break;
        } else {
            cursor->lost_events++;
        }
        SDL_UnlockSpinlock(&slot->lock);
        sequence++;
    }
    if (stop == JCE_TRACE_READ_CAUGHT_UP && sequence <= end && count == max_events)
        stop = JCE_TRACE_READ_BATCH_LIMIT;
    cursor->next_sequence = sequence;
finished:
    if (observation) {
        observation->write_sequence = end;
        observation->earliest_sequence = earliest;
        observation->next_before = before;
        observation->next_after = cursor ? cursor->next_sequence : 0;
        observation->count = count;
        observation->stop = stop;
        observation->blocked_sequence = blocked;
        observation->published_sequence = published_at_stop;
        observation->published_valid = published_valid;
        observation->lost_delta = cursor ? cursor->lost_events - lost_before : 0;
    }
    return count;
}

uint32_t jce_trace_read(JceTraceCursor *cursor, JceTraceEvent *out_events,
                        uint32_t max_events)
{
    return trace_read_impl(cursor, out_events, max_events, NULL);
}

uint32_t jce_trace_read_observed_internal(JceTraceCursor *cursor,
    JceTraceEvent *events, uint32_t maximum, JceTraceReadObservation *observation)
{
    return trace_read_impl(cursor, events, maximum, observation);
}

void jce_trace_get_stats(JceTraceStats *out_stats)
{
    uint32_t written;

    if (!out_stats)
        return;
    SDL_zero(*out_stats);
    written = SDL_GetAtomicU32(&s_write_sequence);
    out_stats->enabled = jce_trace_enabled();
    out_stats->capacity = TRACE_CAPACITY;
    out_stats->resident_events =
        written < TRACE_CAPACITY ? written : TRACE_CAPACITY;
    out_stats->recorded_events = written;
    out_stats->active_threads =
        (uint32_t)SDL_GetAtomicInt(&s_active_threads);
    out_stats->active_work_items =
        (uint32_t)SDL_GetAtomicInt(&s_active_work_items);
    out_stats->active_waits =
        (uint32_t)SDL_GetAtomicInt(&s_active_waits);
    out_stats->tasks_submitted =
        (uint32_t)SDL_GetAtomicInt(&s_tasks_submitted);
    out_stats->work_items_started =
        (uint32_t)SDL_GetAtomicInt(&s_work_items_started);
    out_stats->work_items_completed =
        (uint32_t)SDL_GetAtomicInt(&s_work_items_completed);
    out_stats->waits_completed =
        (uint32_t)SDL_GetAtomicInt(&s_waits_completed);
    out_stats->max_queue_time_ns = trace_duration_load(
        &s_max_queue_ns, &s_max_queue_us, &s_max_queue_ms);
    out_stats->max_run_time_ns = trace_duration_load(
        &s_max_run_ns, &s_max_run_us, &s_max_run_ms);
    out_stats->max_wait_time_ns = trace_duration_load(
        &s_max_wait_ns, &s_max_wait_us, &s_max_wait_ms);
    out_stats->last_wall_frame_ns = trace_u64_load(&s_last_wall_ns);
    out_stats->last_sim_frame_ns = trace_u64_load(&s_last_sim_ns);
    out_stats->last_frame_index = trace_u64_load(&s_last_frame_index);
}

static bool trace_json_reserve(TraceJsonBuffer *buffer, size_t extra)
{
    size_t need;
    size_t capacity;
    char *grown;

    if (extra > SIZE_MAX - buffer->size)
        return false;
    need = buffer->size + extra;
    if (buffer->limit && need > buffer->limit)
        return false;
    if (need <= buffer->capacity)
        return true;
    capacity = buffer->capacity ? buffer->capacity : 4096u;
    while (capacity < need) {
        if (capacity > SIZE_MAX / 2u)
            return false;
        capacity *= 2u;
    }
    if (buffer->limit && capacity > buffer->limit)
        capacity = buffer->limit;
    grown = (char *)JCE_REALLOC(buffer->data, capacity);
    if (!grown)
        return false;
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static bool trace_json_append_n(TraceJsonBuffer *buffer, const char *text,
                                size_t length)
{
    if (length == SIZE_MAX || !trace_json_reserve(buffer, length + 1u))
        return false;
    SDL_memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
    return true;
}

static bool trace_json_append(TraceJsonBuffer *buffer, const char *text)
{
    return trace_json_append_n(buffer, text, SDL_strlen(text));
}

static bool trace_json_append_escaped(TraceJsonBuffer *buffer,
                                      const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (!trace_json_append(buffer, "\""))
        return false;
    while (*cursor) {
        const unsigned char *run = cursor;
        while (*cursor && *cursor >= 0x20u && *cursor != '"' && *cursor != '\\')
            ++cursor;
        if (cursor != run &&
            !trace_json_append_n(buffer, (const char *)run, (size_t)(cursor - run)))
            return false;
        if (!*cursor)
            break;
        char temp[8];
        size_t length;
        if (*cursor == '"' || *cursor == '\\') {
            temp[0] = '\\';
            temp[1] = (char)*cursor;
            length = 2u;
        } else {
            SDL_snprintf(temp, sizeof(temp), "\\u%04x", (unsigned)*cursor);
            length = 6u;
        }
        if (!trace_json_append_n(buffer, temp, length))
            return false;
        ++cursor;
    }
    return trace_json_append(buffer, "\"");
}

static const char *trace_category_name(JceTraceCategory category)
{
    switch (category) {
    case JCE_TRACE_CATEGORY_FRAME:     return "frame";
    case JCE_TRACE_CATEGORY_THREAD:    return "thread";
    case JCE_TRACE_CATEGORY_TASK:      return "task";
    case JCE_TRACE_CATEGORY_WAIT:      return "wait";
    case JCE_TRACE_CATEGORY_RENDER:    return "render";
    case JCE_TRACE_CATEGORY_GPU_SCENE: return "gpu_scene";
    default:                           return "core";
    }
}

typedef struct TraceIntegerArgs {
    char *data;
    size_t size, capacity;
} TraceIntegerArgs;

static bool trace_integer_literal(TraceIntegerArgs *args, const char *text,
                                   size_t length)
{
    if (!args || !args->data || args->size >= args->capacity ||
        length > args->capacity - args->size - 1u)
        return false;
    for (size_t i = 0; i < length; ++i)
        args->data[args->size++] = text[i];
    args->data[args->size] = '\0';
    return true;
}

static bool trace_integer_value(TraceIntegerArgs *args, uint64_t magnitude,
                                 bool negative)
{
    char reversed[20];
    size_t digits = 0;
    do {
        reversed[digits++] = (char)('0' + magnitude % UINT64_C(10));
        magnitude /= UINT64_C(10);
    } while (magnitude);
    const size_t length = digits + (negative ? 1u : 0u);
    if (!args || !args->data || args->size >= args->capacity ||
        length > args->capacity - args->size - 1u)
        return false;
    if (negative) args->data[args->size++] = '-';
    while (digits) args->data[args->size++] = reversed[--digits];
    args->data[args->size] = '\0';
    return true;
}

/* Only integer args use this checked scratch writer. The common timestamp
 * printf remains untouched, including its double conversion and rounding. */
static bool trace_json_append_integer_args(TraceJsonBuffer *json,
    const JceTraceEvent *event, char *scratch, size_t capacity)
{
    TraceIntegerArgs args = {scratch, 0, capacity};
    bool ok;
#define TRACE_ARGS_LITERAL(text_) trace_integer_literal(&args, text_, sizeof(text_) - 1u)
#define TRACE_ARGS_U64(value_) trace_integer_value(&args, (uint64_t)(value_), false)
    if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
        ok = TRACE_ARGS_LITERAL(",\"s\":\"t\",\"args\":{\"task\":") &&
            TRACE_ARGS_U64((unsigned long long)event->id) &&
            TRACE_ARGS_LITERAL(",\"parent\":") &&
            TRACE_ARGS_U64((unsigned long long)event->parent_id) &&
            TRACE_ARGS_LITERAL(",\"kind\":") &&
            TRACE_ARGS_U64((unsigned long long)event->value_u32[0]) &&
            TRACE_ARGS_LITERAL("}}");
    } else if (event->type == JCE_TRACE_EVENT_COUNTER) {
        const int64_t value = (int64_t)event->value_u64;
        const bool negative = value < 0;
        /* Unsigned subtraction handles INT64_MIN without signed overflow. */
        const uint64_t magnitude = negative ? UINT64_C(0) - (uint64_t)value : (uint64_t)value;
        ok = TRACE_ARGS_LITERAL(",\"args\":{\"value\":") &&
            trace_integer_value(&args, magnitude, negative) &&
            TRACE_ARGS_LITERAL(",\"id\":") &&
            TRACE_ARGS_U64((unsigned long long)event->id) &&
            TRACE_ARGS_LITERAL(",\"parent\":") &&
            TRACE_ARGS_U64((unsigned long long)event->parent_id) &&
            TRACE_ARGS_LITERAL("}}");
    } else if (event->type == JCE_TRACE_EVENT_TASK_END) {
        ok = TRACE_ARGS_LITERAL(",\"args\":{\"id\":") &&
            TRACE_ARGS_U64((unsigned long long)event->id) &&
            TRACE_ARGS_LITERAL(",\"parent\":") &&
            TRACE_ARGS_U64((unsigned long long)event->parent_id) &&
            TRACE_ARGS_LITERAL(",\"ns\":") &&
            TRACE_ARGS_U64((unsigned long long)event->value_u64) &&
            TRACE_ARGS_LITERAL(",\"state\":") &&
            TRACE_ARGS_U64((unsigned)event->value_u32[0]) &&
            TRACE_ARGS_LITERAL("}}");
    } else if (event->type == JCE_TRACE_EVENT_TASK_BEGIN ||
               event->type == JCE_TRACE_EVENT_WAIT_BEGIN ||
               event->type == JCE_TRACE_EVENT_WAIT_END) {
        ok = TRACE_ARGS_LITERAL(",\"args\":{\"id\":") &&
            TRACE_ARGS_U64((unsigned long long)event->id) &&
            TRACE_ARGS_LITERAL(",\"parent\":") &&
            TRACE_ARGS_U64((unsigned long long)event->parent_id) &&
            TRACE_ARGS_LITERAL(",\"ns\":") &&
            TRACE_ARGS_U64((unsigned long long)event->value_u64) &&
            TRACE_ARGS_LITERAL("}}");
    } else {
        ok = TRACE_ARGS_LITERAL(",\"s\":\"t\",\"args\":{\"id\":") &&
            TRACE_ARGS_U64((unsigned long long)event->id) &&
            TRACE_ARGS_LITERAL(",\"value\":") &&
            TRACE_ARGS_U64((unsigned long long)event->value_u64) &&
            TRACE_ARGS_LITERAL(",\"aux\":") &&
            TRACE_ARGS_U64((unsigned long long)event->value_u32[0]) &&
            TRACE_ARGS_LITERAL("}}");
    }
#undef TRACE_ARGS_U64
#undef TRACE_ARGS_LITERAL
    return ok && trace_json_append_n(json, args.data, args.size);
}

static bool trace_export_event(TraceJsonBuffer *json,
                               const JceTraceEvent *event,
                               bool *first)
{
    const char *phase;
    const char *event_name = event->name;
    double timestamp_us = (double)event->timestamp_ns / 1000.0;
    char prefix[256];
    char suffix[256];
    int length;

    if (!*first && !trace_json_append(json, ","))
        return false;
    *first = false;

    if (event->type == JCE_TRACE_EVENT_THREAD_REGISTER) {
        length = SDL_snprintf(
            prefix, sizeof(prefix),
            "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
            "\"tid\":%llu,\"args\":{\"name\":",
            (unsigned long long)event->thread_id);
        if (length < 0 || (size_t)length >= sizeof(prefix) ||
            !trace_json_append(json, prefix) ||
            !trace_json_append_escaped(json, event_name) ||
            !trace_json_append(json, "}}"))
            return false;
        return true;
    }

    switch (event->type) {
    case JCE_TRACE_EVENT_TASK_BEGIN:
    case JCE_TRACE_EVENT_WAIT_BEGIN:
        phase = "B";
        break;
    case JCE_TRACE_EVENT_TASK_END:
    case JCE_TRACE_EVENT_WAIT_END:
        phase = "E";
        break;
    case JCE_TRACE_EVENT_COUNTER:
        phase = "C";
        break;
    default:
        phase = "i";
        break;
    }

    if (!trace_json_append(json, "{\"name\":") ||
        !trace_json_append_escaped(json, event_name))
        return false;
    length = SDL_snprintf(
        suffix, sizeof(suffix),
        ",\"cat\":\"%s\",\"ph\":\"%s\",\"pid\":1,\"tid\":%llu,"
        "\"ts\":%.3f",
        trace_category_name(event->category), phase,
        (unsigned long long)event->thread_id, timestamp_us);
    if (length < 0 || (size_t)length >= sizeof(suffix) ||
        !trace_json_append(json, suffix))
        return false;

    return trace_json_append_integer_args(json, event, suffix, sizeof(suffix));
}

uint32_t jce_trace_capture_generation(void)
{
    return (uint32_t)SDL_GetAtomicInt(&s_generation);
}

bool jce_trace_chrome_encode_batch_into(const JceTraceEvent *events,
                                       uint32_t count, bool *first,
                                       TraceJsonBuffer *json)
{
    bool next_first;
    size_t original_size;
    if (!events || !first || !json || json->size > json->capacity ||
        (json->capacity && !json->data) ||
        (json->limit && json->size > json->limit)) return false;
    original_size = json->size;
    next_first = *first;
    for (uint32_t i = 0; i < count; ++i) {
        if (!memchr(events[i].name, '\0', sizeof(events[i].name)) ||
            !trace_export_event(json, &events[i], &next_first)) {
            json->size = original_size;
            if (json->data && json->capacity > original_size)
                json->data[original_size] = '\0';
            return false;
        }
    }
    *first = next_first;
    return true;
}

bool jce_trace_chrome_encode_batch(const JceTraceEvent *events, uint32_t count,
                                  bool *first, char **text, size_t *size)
{
    TraceJsonBuffer json = {0};
    if (!events || !first || !text || !size) return false;
    *text = NULL;
    *size = 0;
    if (!jce_trace_chrome_encode_batch_into(events, count, first, &json)) {
        JCE_FREE(json.data);
        return false;
    }
    *text = json.data;
    *size = json.size;
    return true;
}

static bool trace_export_resident_impl(const char *host_path)
{
    JceTraceEvent *events;
    JceTraceCursor cursor;
    TraceJsonBuffer json;
    uint32_t count;
    uint32_t i;
    uint32_t recorded;
    uint32_t generation;
    uint64_t overwritten;
    char metadata[384];
    int length;
    bool first = true;
    bool ok;

    if (!host_path || !host_path[0] || !jce_trace_capture_valid_internal())
        return false;
    events = JCE_NEW_ARRAY(JceTraceEvent, TRACE_CAPACITY);
    if (!events)
        return false;
    generation = (uint32_t)SDL_GetAtomicInt(&s_generation);
    recorded = SDL_GetAtomicU32(&s_write_sequence);
    overwritten = recorded > TRACE_CAPACITY ? recorded - TRACE_CAPACITY : 0;
    jce_trace_cursor_init(&cursor);
    /* Existing prefix coverage is separate from loss during this read. */
    cursor.next_sequence = (uint32_t)overwritten + 1u;
    count = jce_trace_read(&cursor, events, TRACE_CAPACITY);
    if (cursor.next_sequence > 1u)
        recorded = cursor.next_sequence - 1u;
    SDL_zero(json);
    ok = trace_json_append(&json, "{\"displayTimeUnit\":\"ms\","
                                  "\"traceEvents\":[");
    for (i = 0; ok && i < count; ++i)
        ok = trace_export_event(&json, &events[i], &first);
    if (ok) {
        const bool sequence_valid = jce_trace_capture_valid_internal();
        length = SDL_snprintf(
            metadata, sizeof(metadata),
            "],\"jceTrace\":{\"capacity\":%u,\"recorded\":%u,\"captured\":%u,"
            "\"overwritten_before_capture\":%llu,\"cursor_lost\":%llu,"
            "\"lost\":%llu,\"capture_generation_stable\":%s,\"sequence_exhausted\":%s}}",
            (unsigned)TRACE_CAPACITY, recorded, count, (unsigned long long)overwritten,
            (unsigned long long)cursor.lost_events,
            (unsigned long long)(overwritten + cursor.lost_events),
            generation == (uint32_t)SDL_GetAtomicInt(&s_generation)
                ? "true" : "false", sequence_valid ? "false" : "true");
        ok = sequence_valid && length >= 0 && (size_t)length < sizeof(metadata) &&
            trace_json_append(&json, metadata);
    }
    if (ok)
        ok = jce_trace_capture_valid_internal() &&
            jce_fs_host_write_all(host_path, json.data, (uint64_t)json.size);
    JCE_FREE(json.data);
    JCE_FREE(events);
    return ok && jce_trace_capture_valid_internal();
}

bool jce_trace_export_chrome_json(const char *host_path)
{
    if (!jce_trace_suppress_begin()) return false;
    bool ok = trace_export_resident_impl(host_path);
    if (!jce_trace_suppress_end()) return false;
    return ok;
}
